"""Admin endpoints: SOAP proxy, guide reload, soak control, gate runner, intervention log.

SECURITY CONTRACT:
  - SOAP credentials are read from env vars (config.py) only, never returned to client.
  - Only allowlisted commands reach the worldserver.
  - Playerlike-policy violations (teleport, force-advance, etc.) are blocked.
  - Every command sent is written to the intervention log (JSONL).
"""
from __future__ import annotations

import asyncio
import base64
import json
import logging
import os
import signal
import time
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

from fastapi import APIRouter, Body, HTTPException
from fastapi.responses import JSONResponse

import db
from config import (
    GATE_SCRIPT_DIR,
    GUIDE_RUNTIME_DIR,
    INTERVENTION_LOG_PATH,
    SOAK_LOG_DIR,
    SOAP_HOST,
    SOAP_PASS,
    SOAP_PORT,
    SOAP_USER,
)

log = logging.getLogger(__name__)

router = APIRouter(prefix="/api/admin")

# ─── SOAP allowlists ─────────────────────────────────────────────────────────

# Prefixes of safe, read-only or guide-management commands.
# A command is allowed if it matches one of these prefixes (lowercased, stripped).
_ALLOWED_PREFIXES = [
    ".idlebot status",
    ".idlebot pause ",
    ".idlebot resume ",
    ".idlebot reload guides",
    ".idlebot reload guide ",
    ".idlebot validate guide ",
    ".idlebot list",
    ".idlebot help",
    ".server info",
    ".server uptime",
]

# Dangerous but not outright forbidden — require the caller to set confirm=true.
_DANGEROUS_PREFIXES = [
    ".server restart",
    ".server shutdown",
    ".idlebot reset",
]

# Blocked regardless of confirm — playerlike policy violations or sensitive ops.
_FORBIDDEN_PREFIXES = [
    ".account",
    ".character level",
    ".modify",
    ".tele",
    ".revive",
    ".quest complete",
    ".quest add",
    ".quest remove",
    ".npc add",
    ".lookup account",
    ".gm list",
    ".ban",
    ".idlebot guide jump",
    ".idlebot guide force",
    ".idlebot guide skip",
    ".debug",
    ".cheat",
    ".character delete",
    ".character erase",
]

KNOWN_GATES = [
    "verify_playerlike_policy.sh",
    "verify_idlebot_test_roster_reset.sh",
    "verify_quest_state_integrity.sh",
    "verify_no_invalid_skips.sh",
    "verify_runtime_guides_installed.sh",
    "verify_guide_prereqs.sh",
    "verify_guide_objectives.sh",
    "verify_quest_loot_regression.sh",
]


def _is_allowed(cmd: str) -> tuple[bool, bool, str]:
    """Return (allowed, is_dangerous, reason).

    allowed=True means the command may be forwarded (if is_dangerous, confirm must be true).
    allowed=False means the command is blocked unconditionally.
    """
    lower = cmd.strip().lower()

    for prefix in _FORBIDDEN_PREFIXES:
        if lower.startswith(prefix):
            return False, False, f"forbidden: matches blocked prefix '{prefix}'"

    for prefix in _DANGEROUS_PREFIXES:
        if lower.startswith(prefix):
            return True, True, f"dangerous: '{prefix}' — confirm required"

    for prefix in _ALLOWED_PREFIXES:
        if lower.startswith(prefix):
            return True, False, "ok"

    return False, False, "not in allowlist — add to ALLOWED_PREFIXES to permit"


# ─── SOAP client ─────────────────────────────────────────────────────────────

_SOAP_NS = "urn:AC"
_SOAP_ENV_NS = "http://schemas.xmlsoap.org/soap/envelope/"

_SOAP_TEMPLATE = (
    '<?xml version="1.0" encoding="UTF-8"?>'
    '<SOAP-ENV:Envelope'
    ' xmlns:SOAP-ENV="{env_ns}"'
    ' xmlns:ns1="{ns}"'
    ' xmlns:xsd="http://www.w3.org/2001/XMLSchema"'
    ' xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"'
    ' SOAP-ENV:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">'
    "<SOAP-ENV:Body>"
    "<ns1:executeCommand>"
    '<command xsi:type="xsd:string">{command}</command>'
    "</ns1:executeCommand>"
    "</SOAP-ENV:Body>"
    "</SOAP-ENV:Envelope>"
)

# Module-level state (in-memory, reset on restart)
_soap_state: dict = {
    "reachable": None,
    "last_success_at": None,
    "last_error": None,
    "last_error_at": None,
    "last_command": None,
    "last_latency_ms": None,
}


def _soap_call_sync(command: str) -> tuple[bool, str]:
    """Synchronous SOAP call — run via run_in_executor. Returns (ok, output)."""
    host = SOAP_HOST
    port = SOAP_PORT
    user = SOAP_USER
    password = SOAP_PASS  # noqa: F841 — used for auth, not logged

    url = f"http://{host}:{port}/"
    safe_cmd = command.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    body = _SOAP_TEMPLATE.format(
        env_ns=_SOAP_ENV_NS, ns=_SOAP_NS, command=safe_cmd
    ).encode("utf-8")

    creds = base64.b64encode(f"{user}:{password}".encode()).decode()
    req = urllib.request.Request(
        url,
        data=body,
        headers={
            "Content-Type": "text/xml; charset=utf-8",
            "SOAPAction": "urn:AC#executeCommand",
            "Authorization": f"Basic {creds}",
        },
        method="POST",
    )

    try:
        with urllib.request.urlopen(req, timeout=8) as resp:
            raw = resp.read().decode("utf-8", errors="replace")
        root = ET.fromstring(raw)
        result_el = root.find(".//{urn:AC}result")
        if result_el is None:
            # Try without namespace
            result_el = root.find(".//result")
        output = (result_el.text or "").strip() if result_el is not None else raw.strip()
        return True, output
    except urllib.error.HTTPError as exc:
        return False, f"HTTP {exc.code}: {exc.reason}"
    except urllib.error.URLError as exc:
        return False, f"Connection error: {exc.reason}"
    except ET.ParseError as exc:
        return False, f"XML parse error: {exc}"
    except Exception as exc:  # noqa: BLE001
        return False, f"Error: {exc}"


async def soap_call(command: str) -> tuple[bool, str, float]:
    """Async SOAP call. Returns (ok, output, duration_ms). Redacts creds from logs."""
    log.info("[admin/soap] → %s", command)  # command text only, never user/pass
    t0 = time.monotonic()
    loop = asyncio.get_event_loop()
    ok, output = await loop.run_in_executor(None, _soap_call_sync, command)
    dur = round((time.monotonic() - t0) * 1000, 1)

    now = datetime.now(timezone.utc).isoformat()
    _soap_state["last_command"] = command
    _soap_state["last_latency_ms"] = dur
    if ok:
        _soap_state["reachable"] = True
        _soap_state["last_success_at"] = now
        _soap_state["last_error"] = None
    else:
        _soap_state["reachable"] = False
        _soap_state["last_error"] = output
        _soap_state["last_error_at"] = now

    log.info("[admin/soap] ← ok=%s dur=%.1fms", ok, dur)
    return ok, output, dur


# ─── Intervention log ────────────────────────────────────────────────────────


async def log_intervention(
    action: str,
    command: str,
    result: str,
    detail: str,
    bot: str | None = None,
) -> None:
    entry = {
        "time": datetime.now(timezone.utc).isoformat(),
        "user": "dashboard-admin",
        "action": action,
        "bot": bot,
        "command": command,
        "result": result,
        "detail": detail[:500],
    }
    try:
        log_path = Path(INTERVENTION_LOG_PATH)
        log_path.parent.mkdir(parents=True, exist_ok=True)
        loop = asyncio.get_event_loop()
        await loop.run_in_executor(
            None,
            lambda: log_path.open("a").write(json.dumps(entry) + "\n"),
        )
    except Exception as exc:  # noqa: BLE001
        log.warning("[admin] intervention log write failed: %s", exc)


# ─── Routes ──────────────────────────────────────────────────────────────────


@router.get("/soap/status")
async def soap_status():
    """SOAP reachability + last command info. Never returns credentials."""
    # Do a live ping to get real status
    ok, output, dur = await soap_call(".server uptime")
    uptime_line = output.splitlines()[0] if output else None
    return {
        "reachable": ok,
        "host": SOAP_HOST,
        "port": SOAP_PORT,
        "lastSuccessAt": _soap_state["last_success_at"],
        "lastErrorAt": _soap_state["last_error_at"],
        "lastError": _soap_state["last_error"] if not ok else None,
        "lastCommand": _soap_state["last_command"],
        "lastLatencyMs": dur,
        "uptimeLine": uptime_line if ok else None,
    }


@router.post("/soap/command")
async def run_soap_command(
    command: str = Body(..., embed=True),
    confirm: bool = Body(False, embed=True),
):
    """Proxy an allowlisted SOAP command. Dangerous commands require confirm=true."""
    cmd = command.strip()
    if not cmd:
        raise HTTPException(status_code=400, detail="command is empty")

    allowed, is_dangerous, reason = _is_allowed(cmd)
    if not allowed:
        await log_intervention("soap_command_blocked", cmd, "blocked", reason)
        raise HTTPException(status_code=403, detail=reason)
    if is_dangerous and not confirm:
        return JSONResponse(
            status_code=202,
            content={
                "ok": False,
                "requiresConfirm": True,
                "command": cmd,
                "reason": reason,
            },
        )

    ok, output, dur = await soap_call(cmd)
    await log_intervention(
        "soap_command",
        cmd,
        "ok" if ok else "error",
        output[:400],
    )
    return {"ok": ok, "command": cmd, "output": output, "durationMs": dur}


@router.get("/guides")
async def list_guides():
    """List all active guide IDs and which bots are using each."""
    rows = await db.fetchall(
        "SELECT bot_name, guide_id FROM idlebot_bots WHERE active = 1 AND guide_id IS NOT NULL"
    )
    # Build guide → bots map
    guide_map: dict[str, list[str]] = {}
    for r in rows:
        gid = r["guide_id"]
        if gid:
            guide_map.setdefault(gid, []).append(r["bot_name"])

    guides = []
    for gid, bots in sorted(guide_map.items()):
        # guide_id looks like "alliance/nightelf/00_shadowglen-1-6"
        # derive a display path relative to the runtime dir
        path = f"{gid}.yaml" if not gid.endswith(".yaml") else gid
        guides.append(
            {
                "id": gid,
                "path": path,
                "runtimePath": f"{GUIDE_RUNTIME_DIR}/{path}",
                "activeBots": bots,
            }
        )
    return {"guides": guides, "total": len(guides)}


@router.post("/guides/reload-all")
async def reload_all_guides():
    """Reload all guide YAMLs via SOAP without restarting the worldserver."""
    ok, output, dur = await soap_call(".idlebot reload guides")
    await log_intervention("reload_all_guides", ".idlebot reload guides", "ok" if ok else "error", output[:400])
    return {"ok": ok, "output": output, "durationMs": dur}


@router.post("/guides/{guide_id:path}/reload")
async def reload_guide(guide_id: str):
    """Reload a single guide YAML. guide_id is the runtime-relative path."""
    # Sanitise: prevent path traversal
    safe_id = guide_id.lstrip("/").replace("..", "")
    path = safe_id if safe_id.endswith(".yaml") else f"{safe_id}.yaml"
    cmd = f".idlebot reload guide {path}"
    ok, output, dur = await soap_call(cmd)
    await log_intervention("reload_guide", cmd, "ok" if ok else "error", output[:400], bot=None)
    return {"ok": ok, "guide": path, "output": output, "durationMs": dur}


@router.post("/guides/{guide_id:path}/validate")
async def validate_guide(guide_id: str):
    """Validate a guide file without applying it."""
    safe_id = guide_id.lstrip("/").replace("..", "")
    path = safe_id if safe_id.endswith(".yaml") else f"{safe_id}.yaml"
    cmd = f".idlebot validate guide {path}"
    ok, output, dur = await soap_call(cmd)
    await log_intervention("validate_guide", cmd, "ok" if ok else "error", output[:400])
    return {"ok": ok, "guide": path, "output": output, "durationMs": dur}


@router.get("/soak/status")
async def soak_status():
    """Read soak heartbeat, PID, checkpoint list from the log directory."""
    latest = Path(SOAK_LOG_DIR) / "latest"
    if not latest.exists():
        return {"running": False, "reason": "no soak log directory / latest symlink"}

    pid: int | None = None
    is_running = False
    heartbeat_text: str | None = None
    heartbeat_age_s: float | None = None
    summary: str | None = None
    checkpoints: list[str] = []

    pid_file = latest / "soak.pid"
    if pid_file.exists():
        try:
            pid = int(pid_file.read_text().strip())
            # Check if process is alive
            try:
                os.kill(pid, 0)
                is_running = True
            except (ProcessLookupError, PermissionError):
                is_running = False
        except ValueError:
            pass

    hb_file = latest / "heartbeat.txt"
    if hb_file.exists():
        heartbeat_text = hb_file.read_text().strip()
        try:
            mtime = hb_file.stat().st_mtime
            heartbeat_age_s = round(time.time() - mtime, 1)
        except OSError:
            pass

    summary_file = latest / "summary.md"
    if summary_file.exists():
        try:
            summary = summary_file.read_text()[:2000]
        except OSError:
            pass

    for cp in sorted(latest.glob("checkpoint-*.md")):
        checkpoints.append(cp.name)

    log_dir_name = latest.resolve().name if latest.is_symlink() else None

    return {
        "running": is_running,
        "pid": pid,
        "logDir": str(latest.resolve()) if latest.exists() else None,
        "logDirName": log_dir_name,
        "heartbeatText": heartbeat_text,
        "heartbeatAgeSeconds": heartbeat_age_s,
        "checkpoints": checkpoints,
        "checkpointCount": len(checkpoints),
        "summary": summary,
    }


@router.post("/soak/start")
async def soak_start():
    return JSONResponse(
        status_code=501,
        content={
            "detail": "Start soak via CLI: bash modules/mod-idlebot/tools/start_checkpoint_soak_detached.sh --duration 4h --checkpoint 30m"
        },
    )


@router.post("/soak/stop")
async def soak_stop(confirm: bool = Body(False, embed=True)):
    """Kill the running soak process. Requires confirm=true."""
    if not confirm:
        return JSONResponse(
            status_code=202,
            content={"ok": False, "requiresConfirm": True, "reason": "stopping the soak is a destructive action"},
        )

    pid_file = Path(SOAK_LOG_DIR) / "latest" / "soak.pid"
    if not pid_file.exists():
        raise HTTPException(status_code=404, detail="no soak.pid file — soak may not be running")
    try:
        pid = int(pid_file.read_text().strip())
        os.kill(pid, signal.SIGTERM)
        await log_intervention("soak_stop", f"kill -TERM {pid}", "ok", f"sent SIGTERM to soak PID {pid}")
        return {"ok": True, "pid": pid, "detail": "SIGTERM sent"}
    except (ValueError, ProcessLookupError) as exc:
        raise HTTPException(status_code=409, detail=f"Could not signal soak: {exc}") from exc


@router.post("/bots/{name}/pause")
async def pause_bot(name: str):
    """Pause a bot via SOAP."""
    cmd = f".idlebot pause {name}"
    ok, output, dur = await soap_call(cmd)
    await log_intervention("pause_bot", cmd, "ok" if ok else "error", output[:400], bot=name)
    return {"ok": ok, "bot": name, "output": output, "durationMs": dur}


@router.post("/bots/{name}/resume")
async def resume_bot(name: str):
    """Resume a paused bot via SOAP."""
    cmd = f".idlebot resume {name}"
    ok, output, dur = await soap_call(cmd)
    await log_intervention("resume_bot", cmd, "ok" if ok else "error", output[:400], bot=name)
    return {"ok": ok, "bot": name, "output": output, "durationMs": dur}


@router.post("/bots/{name}/status")
async def bot_status(name: str):
    """Run .idlebot status <name> via SOAP."""
    cmd = f".idlebot status {name}"
    ok, output, dur = await soap_call(cmd)
    return {"ok": ok, "bot": name, "output": output, "durationMs": dur}


# Gate runner ─────────────────────────────────────────────────────────────────

# Cache last run results per gate (in-memory, reset on restart)
_gate_cache: dict[str, dict] = {}


@router.get("/gates")
async def list_gates():
    """List all known gate scripts with their last-run status."""
    gates = []
    for gate in KNOWN_GATES:
        path = os.path.join(GATE_SCRIPT_DIR, gate)
        exists = os.path.exists(path)
        cached = _gate_cache.get(gate, {})
        gates.append(
            {
                "gate": gate,
                "exists": exists,
                "path": path,
                "lastRunAt": cached.get("last_run_at"),
                "exitCode": cached.get("exit_code"),
                "passed": cached.get("exit_code") == 0 if cached.get("exit_code") is not None else None,
                "durationMs": cached.get("duration_ms"),
            }
        )
    return {"gates": gates}


@router.post("/gates/run")
async def run_gate(gate: str = Body(..., embed=True)):
    """Run a specific gate script and return its output."""
    if gate not in KNOWN_GATES:
        raise HTTPException(status_code=400, detail=f"Unknown gate '{gate}'. Allowed: {KNOWN_GATES}")

    script_path = os.path.join(GATE_SCRIPT_DIR, gate)
    if not os.path.exists(script_path):
        raise HTTPException(status_code=404, detail=f"Gate script not found: {script_path}")

    t0 = time.monotonic()
    try:
        proc = await asyncio.create_subprocess_exec(
            "bash", script_path,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        try:
            stdout_bytes, stderr_bytes = await asyncio.wait_for(proc.communicate(), timeout=60)
        except asyncio.TimeoutError:
            proc.kill()
            await proc.communicate()
            raise HTTPException(status_code=504, detail=f"Gate '{gate}' timed out after 60s")
    except HTTPException:
        raise
    except Exception as exc:  # noqa: BLE001
        raise HTTPException(status_code=500, detail=f"Failed to run gate: {exc}") from exc

    dur = round((time.monotonic() - t0) * 1000, 1)
    exit_code = proc.returncode
    stdout = stdout_bytes.decode("utf-8", errors="replace")
    stderr = stderr_bytes.decode("utf-8", errors="replace")
    passed = exit_code == 0
    now = datetime.now(timezone.utc).isoformat()

    _gate_cache[gate] = {
        "last_run_at": now,
        "exit_code": exit_code,
        "duration_ms": dur,
    }

    await log_intervention(
        "run_gate",
        f"bash {gate}",
        "pass" if passed else "fail",
        f"exit={exit_code} dur={dur}ms",
    )

    return {
        "gate": gate,
        "passed": passed,
        "exitCode": exit_code,
        "stdout": stdout[:8000],
        "stderr": stderr[:2000],
        "durationMs": dur,
        "ranAt": now,
    }


@router.get("/interventions")
async def list_interventions(limit: int = 200):
    """Return the last N intervention log entries, newest first."""
    log_path = Path(INTERVENTION_LOG_PATH)
    if not log_path.exists():
        return {"interventions": [], "total": 0}

    entries = []
    try:
        lines = log_path.read_text().splitlines()
        for line in reversed(lines):
            line = line.strip()
            if not line:
                continue
            try:
                entries.append(json.loads(line))
            except json.JSONDecodeError:
                continue
            if len(entries) >= limit:
                break
    except OSError as exc:
        log.warning("[admin] could not read intervention log: %s", exc)

    return {"interventions": entries, "total": len(entries)}
