#!/usr/bin/env python3
"""One-time HERMES owner registration. The desktop app never runs this script."""
import argparse
import getpass
import html
import json
import os
import re
import secrets
import shlex
import subprocess
import time
import urllib.parse
import urllib.request
import webbrowser
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SITE = ROOT / "website"


def post(url, data, bearer=None):
    headers = {"Accept": "application/json", "Content-Type": "application/json", "User-Agent": "HERMES registration"}
    if bearer:
        headers["Authorization"] = "Bearer " + bearer
    req = urllib.request.Request(url, data=json.dumps(data).encode(), headers=headers, method="POST")
    with urllib.request.urlopen(req, timeout=30) as response:
        return json.load(response)


def github(manifest):
    state = secrets.token_urlsafe(32)
    result = {}

    class Callback(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass  # Callback URLs contain a single-use credential.

        def reply(self, content):
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Referrer-Policy", "no-referrer")
            self.end_headers()
            self.wfile.write(content.encode())

        def do_GET(self):
            parts = urllib.parse.urlsplit(self.path)
            if parts.path == "/" + state:
                action = "https://github.com/settings/apps/new?state=" + state
                value = html.escape(json.dumps(manifest), quote=True)
                self.reply(f'<title>Register HERMES</title><form method="post" action="{action}">'
                           f'<input type="hidden" name="manifest" value="{value}">'
                           '<button>Create the HERMES GitHub App</button></form>')
            elif parts.path == "/callback":
                args = urllib.parse.parse_qs(parts.query)
                code = args.get("code", [""])[0]
                if args.get("state") != [state] or not re.fullmatch(r"[a-zA-Z0-9_-]+", code):
                    self.send_error(400)
                    return
                result["code"] = code
                self.reply("Registration approved. Return to the terminal.")
            else:
                self.send_error(404)

    with HTTPServer(("127.0.0.1", 0), Callback) as server:
        base = f"http://127.0.0.1:{server.server_port}"
        manifest["redirect_url"] = base + "/callback"
        webbrowser.open(base + "/" + state)
        print("Approve the new app in your browser.", flush=True)
        end = time.monotonic() + 3600
        server.timeout = 1
        while not result and time.monotonic() < end:
            server.handle_request()
    if not result:
        raise SystemExit("Registration timed out.")
    data = post("https://api.github.com/app-manifests/" + result["code"] + "/conversions", {})
    return {"CLIENT_ID": data["client_id"], "CLIENT_SECRET": data["client_secret"], "APP_SLUG": data["slug"]}


def slack(manifest):
    token = os.getenv("SLACK_CONFIGURATION_TOKEN") or getpass.getpass("Owner's Slack app configuration token: ")
    data = post("https://slack.com/api/apps.manifest.create", {"manifest": json.dumps(manifest)}, token)
    if not data.get("ok"):
        raise SystemExit("Slack registration failed: " + data.get("error", "unknown error"))
    creds = data["credentials"]
    return {"CLIENT_ID": creds["client_id"], "CLIENT_SECRET": creds["client_secret"]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("provider", choices=["github", "slack"])
    provider = parser.parse_args().provider
    vc = shlex.split(os.getenv("HERMES_VERCEL_CLI", "vercel"))
    subprocess.run(vc + ["env", "ls", "--cwd", str(SITE)], check=True)
    manifest = json.loads((ROOT / "docs/oauth" / (provider + ".json")).read_text())
    values = github(manifest) if provider == "github" else slack(manifest)
    for key, value in values.items():
        args = vc + ["env", "add", f"HERMES_{provider.upper()}_{key}", "production", "--cwd", str(SITE)]
        if key == "CLIENT_SECRET":
            args.append("--sensitive")
        subprocess.run(args, input=value.encode(), check=True)
    subprocess.run(vc + ["deploy", "--prod", "--yes", "--cwd", str(SITE)], check=True)
