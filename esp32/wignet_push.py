#!/usr/bin/env python3
"""Submit your own build to your Wignet account, to install it over the air.

    pio run -e <environment>
    python3 wignet_push.py -e <environment> [--changelog "What changed"]

Then install it from your Wignet's page (Settings › Firmware › Your firmware)
or with `update_firmware` on the account connector. docs/own_firmware.md says
what to keep, what the server refuses and why.

Before uploading, it checks what the server checks (docs/ota.md in the wignet
repository, "Who certifies, who installs"), so a refusal comes with no
network round trip:
- the image reports a clean commit (`&fw=<commit>`), not `-dirty` or
  `unknown`: the server confirms an update only by its commit, so such a
  build would roll back by itself after ten minutes;
- the image declares `ota1`: without the OTA client the board would take no
  update again until a USB flash;
- the environment is not an `_ota1_bench` one, whose key is the bench's.

Sign-in: the upload needs an account access token, the same one the account
connector (`https://app.wignet.app/mcp/account`) uses. The first run opens
your browser to sign in to Wignet (OAuth with PKCE, a client registered for
this script) and keeps the token in ~/.config/wignet/push.json (only you can
read it); later runs refresh it. `--logout` forgets it. A token from elsewhere
can be passed as WIGNET_TOKEN instead.

Python standard library only. WIGNET_SERVER overrides https://app.wignet.app.
"""

from __future__ import annotations

import argparse
import base64
import configparser
import hashlib
import http.server
import json
import os
import re
import secrets
import socket
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import webbrowser
from pathlib import Path

HERE = Path(__file__).resolve().parent
SERVER = os.environ.get("WIGNET_SERVER", "https://app.wignet.app").rstrip("/")
CACHE = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "wignet" / "push.json"
MAX_IMAGE = 8 * 1024 * 1024  # an app image, as the server takes it

# What the server reads in the image (backoffice/firmware_store.py and
# firmware_release.py in the wignet repository): keep in step.
IMAGE_VERSION = re.compile(rb"&fw=([0-9a-z.-]{1,64})\x00")
DECLARES_OTA1 = (b"%2Cota1", b"v1%3Aota1")
HW_TARGET = re.compile(r'-DWIGNET_HW_TARGET=\\"([A-Za-z0-9_]+)\\"')


class Refused(Exception):
    """A reason not to upload, for the user."""


# --- the build ---------------------------------------------------------------

def board_of(environment: str) -> str:
    """The `hw=` this environment's build reports: its name, unless its own
    section fixes WIGNET_HW_TARGET (firmware_version.py)."""
    config = configparser.ConfigParser(interpolation=None, strict=False)
    config.read(HERE / "platformio.ini")
    section = f"env:{environment}"
    if not config.has_section(section):
        raise Refused(f"no [env:{environment}] in platformio.ini")
    found = HW_TARGET.search(config.get(section, "build_flags", fallback=""))
    return found.group(1) if found else environment


def generated_version() -> str | None:
    header = HERE / "src" / "firmware_version_gen.h"
    if not header.exists():
        return None
    found = re.search(r'WIGNET_FW_VERSION "([^"]*)"', header.read_text())
    return found.group(1) if found else None


def check_image(environment: str, data: bytes) -> str:
    """The commit the image reports, or Refused."""
    if environment.endswith("_ota1_bench"):
        raise Refused(f"{environment} is a bench build: it trusts the bench's test key, not the "
                      "server's, so once installed it could not update over the air again. "
                      "Build the board's own environment instead")
    if len(data) > MAX_IMAGE:
        raise Refused(f"the image is {len(data)} bytes; an app image is at most {MAX_IMAGE}")
    commit_hint = ("Commit your changes (git commit), build again "
                   f"(pio run -e {environment}) and run this again")
    # The header is what the last build wrote, for any environment; the image
    # is what would be installed. Check both, the image decides.
    header = generated_version()
    if header is not None and ("-dirty" in header or header == "unknown"):
        raise Refused(f"src/firmware_version_gen.h says {header}: the last build was from "
                      f"uncommitted changes. {commit_hint}")
    versions = {m.group(1).decode() for m in IMAGE_VERSION.finditer(data)}
    if len(versions) != 1:
        raise Refused("this image carries no single &fw= version: build it with this PlatformIO "
                      "project, which stamps the commit")
    version = versions.pop()
    if "-dirty" in version or version == "unknown":
        raise Refused(f"this image was built from uncommitted changes ({version}). The server "
                      "confirms an update only when the board reports the commit it was stored "
                      "with, so this one would roll back by itself after ten minutes. " + commit_hint)
    if not any(token in data for token in DECLARES_OTA1):
        raise Refused("this image does not declare ota1: installed, it could not update over the "
                      "air again (only USB would). Keep the OTA client (WIGNET_OTA_CLIENT, "
                      "WIGNET_OTA_CAPABLE) and src/ota_keys.h as they are")
    return version


# --- sign-in -----------------------------------------------------------------

def http_json(url: str, data: dict | None = None, headers: dict | None = None) -> dict:
    body = None
    headers = dict(headers or {})
    if data is not None:
        if headers.get("Content-Type") == "application/json":
            body = json.dumps(data).encode()
        else:
            body = urllib.parse.urlencode(data).encode()
            headers["Content-Type"] = "application/x-www-form-urlencoded"
    headers.setdefault("Accept", "application/json")
    request = urllib.request.Request(url, data=body, headers=headers)
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.loads(response.read() or b"{}")


def discover() -> tuple[str, dict]:
    """The account connector's resource and its authorization server."""
    resource = http_json(f"{SERVER}/.well-known/oauth-protected-resource/mcp/account")
    issuer = resource["authorization_servers"][0].rstrip("/")
    return resource["resource"], http_json(f"{issuer}/.well-known/oauth-authorization-server")


def load_cache() -> dict:
    try:
        cache = json.loads(CACHE.read_text())
    except (OSError, ValueError):
        return {}
    return cache if cache.get("server") == SERVER else {}


def save_cache(cache: dict) -> None:
    CACHE.parent.mkdir(parents=True, exist_ok=True)
    CACHE.touch(mode=0o600, exist_ok=True)
    os.chmod(CACHE, 0o600)
    CACHE.write_text(json.dumps({**cache, "server": SERVER}, indent=2))


def keep_tokens(cache: dict, tokens: dict) -> str:
    cache["access_token"] = tokens["access_token"]
    cache["expires_at"] = time.time() + int(tokens.get("expires_in", 300)) - 30
    if tokens.get("refresh_token"):
        cache["refresh_token"] = tokens["refresh_token"]
    save_cache(cache)
    return cache["access_token"]


def free_port(preferred: int | None) -> int:
    for port in ([preferred] if preferred else []) + [0]:
        with socket.socket() as probe:
            try:
                probe.bind(("127.0.0.1", port))
            except OSError:
                continue
            return probe.getsockname()[1]
    raise Refused("no free local port for the sign-in callback")


def sign_in(cache: dict, resource: str, server: dict) -> str:
    """Browser sign-in, authorization code with PKCE, a loopback redirect."""
    port = free_port(cache.get("port"))
    redirect = f"http://127.0.0.1:{port}/callback"
    if cache.get("redirect_uri") != redirect or not cache.get("client_id"):
        client = http_json(server["registration_endpoint"], {
            "client_name": "Wignet firmware push",
            "redirect_uris": [redirect],
            "grant_types": ["authorization_code", "refresh_token"],
            "response_types": ["code"],
            "token_endpoint_auth_method": "none",
        }, {"Content-Type": "application/json"})
        cache.update(client_id=client["client_id"], redirect_uri=redirect, port=port)
        cache.pop("refresh_token", None)
        save_cache(cache)

    verifier = secrets.token_urlsafe(48)
    challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()
    state = secrets.token_urlsafe(16)
    result: dict = {}

    class Callback(http.server.BaseHTTPRequestHandler):
        def do_GET(self):  # noqa: N802 - http.server's name
            query = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
            if urllib.parse.urlparse(self.path).path != "/callback":
                self.send_error(404)
                return
            result.update({key: values[0] for key, values in query.items()})
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.end_headers()
            self.wfile.write("Signed in. You can close this tab and go back to the terminal.\n".encode())

        def log_message(self, *args):
            pass

    httpd = http.server.HTTPServer(("127.0.0.1", port), Callback)
    threading.Thread(target=httpd.handle_request, daemon=True).start()
    url = server["authorization_endpoint"] + "?" + urllib.parse.urlencode({
        "response_type": "code",
        "client_id": cache["client_id"],
        "redirect_uri": redirect,
        "scope": "openid offline_access",
        "code_challenge": challenge,
        "code_challenge_method": "S256",
        "state": state,
        "resource": resource,
    })
    print("Sign in to Wignet in your browser. If it does not open, open:\n  " + url, file=sys.stderr)
    webbrowser.open(url)
    deadline = time.time() + 300
    while not result and time.time() < deadline:
        time.sleep(0.2)
    httpd.server_close()
    if not result:
        raise Refused("no sign-in within five minutes")
    if result.get("state") != state or "code" not in result:
        raise Refused(f"sign-in failed: {result.get('error_description') or result.get('error') or 'bad state'}")
    tokens = http_json(server["token_endpoint"], {
        "grant_type": "authorization_code",
        "code": result["code"],
        "redirect_uri": redirect,
        "client_id": cache["client_id"],
        "code_verifier": verifier,
        "resource": resource,
    })
    return keep_tokens(cache, tokens)


def access_token() -> str:
    if os.environ.get("WIGNET_TOKEN"):
        return os.environ["WIGNET_TOKEN"]
    cache = load_cache()
    if cache.get("access_token") and cache.get("expires_at", 0) > time.time():
        return cache["access_token"]
    resource, server = discover()
    if cache.get("refresh_token") and cache.get("client_id"):
        try:
            return keep_tokens(cache, http_json(server["token_endpoint"], {
                "grant_type": "refresh_token",
                "refresh_token": cache["refresh_token"],
                "client_id": cache["client_id"],
                "resource": resource,
            }))
        except urllib.error.HTTPError:
            cache.pop("refresh_token", None)
    return sign_in(cache, resource, server)


# --- the upload --------------------------------------------------------------

def multipart(fields: list[tuple[str, str]], image: bytes) -> tuple[bytes, str]:
    boundary = uuid.uuid4().hex
    parts = []
    for name, value in fields:
        parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"\r\n\r\n'
                     .encode() + value.encode() + b"\r\n")
    parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="image"; filename="firmware.bin"\r\n'
                 "Content-Type: application/octet-stream\r\n\r\n".encode() + image + b"\r\n")
    parts.append(f"--{boundary}--\r\n".encode())
    return b"".join(parts), f"multipart/form-data; boundary={boundary}"


def upload(token: str, board: str, image: bytes, changelog: str | None) -> tuple[int, dict]:
    fields = [("board", board)] + ([("changelog", changelog)] if changelog else [])
    body, content_type = multipart(fields, image)
    request = urllib.request.Request(f"{SERVER}/api/account/firmware", data=body, method="POST", headers={
        "Authorization": f"Bearer {token}", "Content-Type": content_type, "Accept": "application/json"})
    try:
        with urllib.request.urlopen(request, timeout=300) as response:
            return response.status, json.loads(response.read() or b"{}")
    except urllib.error.HTTPError as error:
        raw = error.read()
        try:
            return error.code, json.loads(raw)
        except ValueError:
            return error.code, {"error": raw.decode(errors="replace")[:500] or error.reason}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("-e", "--environment", help="the PlatformIO environment you built")
    parser.add_argument("--changelog", help="what this build changes, shown with it (Markdown)")
    parser.add_argument("--check", action="store_true", help="only check the image, upload nothing")
    parser.add_argument("--logout", action="store_true", help="forget the saved sign-in")
    args = parser.parse_args()

    if args.logout:
        CACHE.unlink(missing_ok=True)
        print("Signed out: the next upload asks you to sign in again.")
        return 0
    if not args.environment:
        parser.error("-e/--environment is required")
    image_path = HERE / ".pio" / "build" / args.environment / "firmware.bin"
    try:
        if not image_path.exists():
            raise Refused(f"no {image_path.relative_to(HERE)}: build it first (pio run -e {args.environment})")
        image = image_path.read_bytes()
        board = board_of(args.environment)
        commit = check_image(args.environment, image)
        print(f"{image_path.relative_to(HERE)}: board {board}, commit {commit}, {len(image)} bytes")
        if args.check:
            return 0
        status, answer = upload(access_token(), board, image, args.changelog)
        if status == 401 and not os.environ.get("WIGNET_TOKEN"):
            # A token the server no longer takes (revoked, another environment):
            # sign in again once.
            cache = load_cache()
            cache.pop("access_token", None)
            cache.pop("refresh_token", None)
            save_cache(cache)
            status, answer = upload(access_token(), board, image, args.changelog)
    except Refused as reason:
        print(f"Not uploaded: {reason}.", file=sys.stderr)
        return 1
    except (urllib.error.URLError, OSError, KeyError, ValueError) as error:
        print(f"Not uploaded: {error}", file=sys.stderr)
        return 1
    if status >= 400:
        print(f"Refused by the server ({status}): {answer.get('error', answer)}", file=sys.stderr)
        return 1
    short_id = answer.get("short_id")
    print(f"Submitted: {short_id}")
    if answer.get("note"):
        print(answer["note"])
    print("Install it on a Wignet you administer:\n"
          "  - its page: Settings › Firmware › Your firmware, then Install;\n"
          f"  - or ask your agent, on the account connector: update_firmware with firmware_id {short_id}.\n"
          "It installs at the board's next sync and must confirm within ten minutes, or the board "
          "goes back to the firmware it had.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
