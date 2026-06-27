#!/usr/bin/env python3
"""SOAP admin client for AzerothCore worldserver.

Sends GM commands to the worldserver via SOAP without needing an in-game session.
Requires SOAP to be enabled in worldserver.conf:
    SOAP.Enabled = 1
    SOAP.IP = "0.0.0.0"
    SOAP.Port = 7878

The SOAP account must have GM level >= 3 (GM). Use the KHUONG account or create
a dedicated soapadmin account via the game or console.

Usage:
    python3 tools/soap_admin.py --cmd ".idlebot status"
    python3 tools/soap_admin.py --cmd ".idlebot guide set Idlemage horde-undead-1-18"
    python3 tools/soap_admin.py --cmd ".idlebot pause Idlemage"
    python3 tools/soap_admin.py --cmd ".idlebot resume Idlemage"
    python3 tools/soap_admin.py --interactive

    # Environment variables (avoids putting credentials in shell history):
    export SOAP_USER=KHUONG
    export SOAP_PASS=yourpassword
    python3 tools/soap_admin.py --cmd ".idlebot status"

    # Or pass credentials inline:
    python3 tools/soap_admin.py --user KHUONG --password yourpassword --cmd ".idlebot status"

Useful idlebot commands:
    .idlebot status                         — list all registered bots
    .idlebot status <bot>                   — detailed status for one bot
    .idlebot guide set <bot> <guide>        — assign guide (smart-skips done quests)
    .idlebot guide set <bot> <guide> <step> — assign guide at specific step (0-indexed)
    .idlebot guide current <bot>            — show current guide + step
    .idlebot guide reset <bot>              — reset to step 0
    .idlebot pause <bot>                    — pause bot (stops executing steps)
    .idlebot resume <bot>                   — resume paused bot
    .idlebot reload                         — reload all guide YAML files
    .idlebot add <bot>                      — register a bot
    .idlebot remove <bot>                   — deregister a bot
"""

import argparse
import os
import sys
import urllib.request
import urllib.error
import base64
import xml.etree.ElementTree as ET

SOAP_HOST = os.environ.get("SOAP_HOST", "10.10.30.20")
SOAP_PORT = int(os.environ.get("SOAP_PORT", "7878"))
SOAP_USER = os.environ.get("SOAP_USER", "")
SOAP_PASS = os.environ.get("SOAP_PASS", "")

SOAP_NS = "urn:AC"
SOAP_ENV_NS = "http://schemas.xmlsoap.org/soap/envelope/"

REQUEST_TEMPLATE = """<?xml version="1.0" encoding="UTF-8"?>
<SOAP-ENV:Envelope
  xmlns:SOAP-ENV="{env_ns}"
  xmlns:ns1="{ns}"
  xmlns:xsd="http://www.w3.org/2001/XMLSchema"
  xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
  SOAP-ENV:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">
  <SOAP-ENV:Body>
    <ns1:executeCommand>
      <command xsi:type="xsd:string">{command}</command>
    </ns1:executeCommand>
  </SOAP-ENV:Body>
</SOAP-ENV:Envelope>"""


def send_command(command: str, user: str, password: str, host: str = SOAP_HOST, port: int = SOAP_PORT) -> str:
    url = f"http://{host}:{port}/"
    body = REQUEST_TEMPLATE.format(
        env_ns=SOAP_ENV_NS,
        ns=SOAP_NS,
        command=command.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"),
    ).encode("utf-8")

    creds = base64.b64encode(f"{user}:{password}".encode()).decode()
    req = urllib.request.Request(
        url,
        data=body,
        headers={
            "Content-Type": "text/xml; charset=utf-8",
            "SOAPAction": "urn:TC#executeCommand",
            "Authorization": f"Basic {creds}",
        },
        method="POST",
    )

    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            raw = resp.read()
    except urllib.error.HTTPError as e:
        return f"HTTP {e.code}: {e.reason}"
    except urllib.error.URLError as e:
        return f"Connection error: {e.reason}"

    try:
        root = ET.fromstring(raw)
        ns_map = {"soap": SOAP_ENV_NS, "ns1": SOAP_NS}
        result_el = root.find(".//result")
        if result_el is None:
            result_el = root.find(".//{urn:TC}result")
        if result_el is not None and result_el.text:
            return result_el.text.strip()
        return raw.decode("utf-8", errors="replace")
    except ET.ParseError:
        return raw.decode("utf-8", errors="replace")


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--host", default=SOAP_HOST)
    parser.add_argument("--port", type=int, default=SOAP_PORT)
    parser.add_argument("--user", default=SOAP_USER,
                        help="GM account username (or set SOAP_USER env)")
    parser.add_argument("--password", default=SOAP_PASS,
                        help="GM account password (or set SOAP_PASS env)")
    parser.add_argument("--cmd", help="Single command to run")
    parser.add_argument("--interactive", "-i", action="store_true",
                        help="Read commands from stdin line by line")
    args = parser.parse_args()

    if not args.user or not args.password:
        print("ERROR: SOAP credentials required.", file=sys.stderr)
        print("Set SOAP_USER and SOAP_PASS environment variables, or use --user/--password.", file=sys.stderr)
        print("Example: export SOAP_USER=KHUONG && export SOAP_PASS=yourpassword", file=sys.stderr)
        return 1

    def run(cmd):
        result = send_command(cmd, args.user, args.password, args.host, args.port)
        print(result)

    if args.cmd:
        run(args.cmd)
    elif args.interactive:
        print(f"SOAP admin connected to {args.host}:{args.port} as {args.user}")
        print("Type GM commands (e.g. '.idlebot status'). Ctrl+D to quit.")
        try:
            for line in sys.stdin:
                line = line.strip()
                if line:
                    run(line)
        except KeyboardInterrupt:
            pass
    else:
        parser.print_help()
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
