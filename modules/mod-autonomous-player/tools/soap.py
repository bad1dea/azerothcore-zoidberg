#!/usr/bin/env python3
"""Tiny SOAP console client for ad-hoc worldserver commands.

Usage:
    AP_SOAP_HOST=10.10.30.20 AP_SOAP_USER=... AP_SOAP_PASSWORD=... \
        python3 soap.py ".autonomousplayer status"

Credentials come from AP_SOAP_* env vars (never hardcoded / checked in).
Thin wrapper around soap_command in live_regression_suite.py.
"""
from __future__ import annotations

import os
import sys

from live_regression_suite import Config, soap_command


def main() -> int:
    cmd = " ".join(sys.argv[1:])
    if not cmd:
        print("usage: soap.py <console command>", file=sys.stderr)
        return 2
    cfg = Config(
        host=os.environ.get("AP_SOAP_HOST", "127.0.0.1"),
        port=int(os.environ.get("AP_SOAP_PORT", "7878")),
        user=os.environ.get("AP_SOAP_USER", ""),
        password=os.environ.get("AP_SOAP_PASSWORD", ""),
        bot_account="",
        bot_char="",
        creature_entry=0,
    )
    print(soap_command(cfg, cmd, timeout=30.0))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
