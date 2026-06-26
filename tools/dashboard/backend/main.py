"""IdleBot Live Ops Dashboard — FastAPI backend entry point.

Start with:
    uvicorn main:app --reload --port 8765
"""
import asyncio
import logging
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import HTMLResponse
from fastapi.staticfiles import StaticFiles

import db
from routers import bots as bots_router
from routers import ws as ws_router
from routers import admin as admin_router

log = logging.getLogger(__name__)

_STATIC_ADMIN_DIR = Path(__file__).parent / "static" / "admin"


@asynccontextmanager
async def lifespan(app: FastAPI):
    await db.init_pool()
    # Start background WebSocket broadcast loop
    task = asyncio.create_task(ws_router.snapshot_loop())

    # Warm SOAP state on startup (non-fatal if worldserver is down)
    try:
        from routers.admin import soap_call
        ok, output, _ = await soap_call(".server uptime")
        if ok:
            log.info("[admin] SOAP reachable at startup: %s", output.splitlines()[0] if output else "ok")
        else:
            log.warning("[admin] SOAP not reachable at startup: %s", output)
    except Exception as exc:  # noqa: BLE001
        log.warning("[admin] SOAP startup ping failed: %s", exc)

    yield

    task.cancel()
    try:
        await task
    except asyncio.CancelledError:
        pass
    await db.close_pool()


app = FastAPI(
    title="IdleBot Dashboard API",
    version="1.0.0",
    description="Live ops dashboard for AzerothCore IdleBot playerbot characters.",
    lifespan=lifespan,
)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=False,
    allow_methods=["GET", "POST", "OPTIONS"],
    allow_headers=["*"],
)

app.include_router(bots_router.router)
app.include_router(ws_router.router)
app.include_router(admin_router.router)


@app.get("/healthz")
async def healthz():
    return {"status": "ok"}


# Serve the static admin UI at /admin (fallback for non-React environments)
@app.get("/admin", response_class=HTMLResponse)
@app.get("/admin/", response_class=HTMLResponse)
async def admin_ui():
    html_path = _STATIC_ADMIN_DIR / "index.html"
    if html_path.exists():
        return HTMLResponse(content=html_path.read_text())
    return HTMLResponse(
        content="<h1>Admin UI not built</h1><p>index.html not found at static/admin/index.html</p>",
        status_code=404,
    )


# Mount static assets (JS, CSS, images if needed) under /admin/static
_admin_assets = _STATIC_ADMIN_DIR / "assets"
if _admin_assets.exists():
    app.mount("/admin/assets", StaticFiles(directory=str(_admin_assets)), name="admin-assets")
