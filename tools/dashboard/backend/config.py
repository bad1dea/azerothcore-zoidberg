import os
from dotenv import load_dotenv

load_dotenv()

# Default: the Docker bridge IP on zoidberg (correct when backend runs on zoidberg itself).
# Override with IDLEBOT_DB_HOST=127.0.0.1 if using an SSH tunnel:
#   ssh -L 3306:10.99.0.11:3306 khuong@10.10.30.20
DB_HOST = os.getenv("IDLEBOT_DB_HOST", "10.99.0.11")
DB_PORT = int(os.getenv("IDLEBOT_DB_PORT", "3306"))
DB_USER = os.getenv("IDLEBOT_DB_USER", "root")
DB_PASS = os.getenv("IDLEBOT_DB_PASS", "36411d327aedd4204ec620398ab0d30e7ef0ecc68fbaea53")
DB_NAME = os.getenv("IDLEBOT_DB_NAME", "acore_characters")
DASHBOARD_PORT = int(os.getenv("DASHBOARD_PORT", "8765"))

# Seconds between WebSocket snapshot pushes
WS_INTERVAL = float(os.getenv("WS_INTERVAL", "2.0"))

# SOAP admin — credentials are server-side only, never sent to the browser
SOAP_HOST = os.getenv("ACORE_SOAP_HOST", "10.10.30.20")
SOAP_PORT = int(os.getenv("ACORE_SOAP_PORT", "7878"))
SOAP_USER = os.getenv("ACORE_SOAP_USER", "KHUONG")
SOAP_PASS = os.getenv("ACORE_SOAP_PASS", "KHUONG1234")

# Paths — guide runtime dir is inside the Docker container (not readable by the backend)
GUIDE_RUNTIME_DIR = os.getenv(
    "IDLEBOT_GUIDE_RUNTIME_DIR",
    "/azerothcore/modules/mod-idlebot/data/guides",
)
SOAK_LOG_DIR = os.getenv(
    "IDLEBOT_SOAK_LOG_DIR",
    "/home/khuong/build/azerothcore-zoidberg/modules/mod-idlebot/logs/idlebot-soak",
)
GATE_SCRIPT_DIR = os.getenv(
    "IDLEBOT_GATE_SCRIPT_DIR",
    "/home/khuong/build/azerothcore-zoidberg/modules/mod-idlebot/tools",
)
INTERVENTION_LOG_PATH = os.getenv(
    "IDLEBOT_INTERVENTION_LOG",
    "/home/khuong/build/azerothcore-zoidberg/modules/mod-idlebot/logs/interventions.jsonl",
)
