"""Write an ignored board-local bridge setting. Read secrets from the environment only."""
import argparse
import json
import os
from pathlib import Path
from urllib.parse import urlsplit

parser = argparse.ArgumentParser()
parser.add_argument("--url", required=True)
parser.add_argument("--token-env", default="SIM_BRIDGE_TOKEN")
args = parser.parse_args()
url = urlsplit(args.url)
if (url.scheme not in ("http", "https") or not url.hostname or url.username or url.password
        or url.query or url.fragment or url.path not in ("", "/")
        or url.hostname in ("localhost", "::1") or url.hostname.startswith("127.")
        or len(args.url.encode()) > 240):
    parser.error("Use a computer LAN HTTP root URL, not localhost or 127.0.0.1")
token = os.environ.get(args.token_env, "")
if not token or len(token.encode()) > 256 or any(ord(c) < 32 or ord(c) == 127 for c in token):
    parser.error("Set the token environment variable to a nonempty value (max 256 bytes)")
target = Path(__file__).resolve().parents[1] / "main/boards/waveshare/apple-lamp-ov3660/lamp_bridge_local.h"
target.write_text("// Local setting: ignored by Git. Never commit.\n#pragma once\n"
                  f"#define APPLE_LAMP_BRIDGE_URL {json.dumps(args.url.rstrip('/'))}\n"
                  f"#define APPLE_LAMP_BRIDGE_TOKEN {json.dumps(token)}\n", encoding="utf-8")
# A previously absent __has_include file is not in Ninja's old dependency graph.
# Invalidate this board object without changing tracked source contents.
os.utime(target.with_name("lamp_bridge.cc"), None)
print("Board-local bridge settings written; token redacted. Rebuild to apply.")
