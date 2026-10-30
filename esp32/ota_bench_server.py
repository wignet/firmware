#!/usr/bin/env python3
"""A local OTA server for the bench (docs/ota_bench.md).

It speaks the contract of wignet's docs/ota.md to a bench build of the
PhotoPainter (env waveshare_photopainter_73e6_ota1_bench) or the 7.5 V2
(waveshare_driver_75v2_ota1_bench): a minimal /schedule
over HTTP that offers one image and confirms it, and /firmware/<sha256>.bin
over HTTPS. Each run can inject one fault, to check that the board refuses,
reports or rolls back as the contract says.

    python ota_bench_server.py init --ip 192.168.1.20
    OTA_BENCH_TAG=a pio run -e waveshare_photopainter_73e6_ota1_bench -t upload
    OTA_BENCH_TAG=b pio run -e waveshare_photopainter_73e6_ota1_bench
    python ota_bench_server.py serve --image .pio/build/waveshare_photopainter_73e6_ota1_bench/firmware.bin --fault expired

`init` writes bench/ (git-ignored): a test signing key, a bench CA and the
server's certificate, an untrusted CA for the TLS fault, and ota_bench.h for
the firmware. Nothing in it is a production secret, and nothing is committed.
Needs the `cryptography` package.
"""
import argparse
import base64
import datetime
import hashlib
import http.server
import ipaddress
import json
import pathlib
import secrets
import socketserver
import ssl
import sys
import threading
import time
import urllib.parse

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

BENCH = pathlib.Path(__file__).resolve().parent / "bench"
OFFER_DOMAIN = "wignet-ota1"
CONFIRM_DOMAIN = "wignet-ota-confirm1"
# Offers and confirmations are signed for the board that polls (its hw=):
# one bench serves the PhotoPainter and the 7.5 V2. wrong_hw names this one.
OTHER_HW = "waveshare_esp32s3_lcd_2"
# One fault per run. Each says what the board should do (docs/ota.md).
FAULTS = {
    "none": "valid offer and confirmation: installs, then confirmed:<id>",
    "expired": "offer expired an hour ago: refused, 'expired'",
    "wrong_hw": "offer signed for another board: refused, 'hw is not this board'",
    "stale_nonce": "offer signed for the previous poll's nonce: refused, 'signature'",
    "bad_sig": "offer signed by an unknown key under the bench key's id: refused, 'signature'",
    "wrong_counter": "offer reuses the last counter: refused, 'counter not above the last attempt'",
    "wrong_hash": "signed sha256 is not the served image's: failed:<id>:hash",
    "wrong_size": "signed size is 16 bytes more than served: failed:<id>:size",
    "truncated": "download cut halfway: failed:<id>:download",
    "bad_tls": "firmware served with a certificate from an untrusted CA: failed:<id>:download",
    "no_confirm": "no otaConfirm: the pending image rolls back at its 10-minute deadline",
    "forged_confirm": "otaConfirm signed by an unknown key: no confirmation, rollback",
    "replayed_confirm": "otaConfirm for the previous poll's nonce: no confirmation, rollback",
}


def now():
    return datetime.datetime.now(datetime.timezone.utc)


def pem(key):
    return key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                             serialization.NoEncryption())


def write_private(path, data):
    path.write_bytes(data)
    path.chmod(0o600)


def make_ca(name):
    key = ec.generate_private_key(ec.SECP256R1())
    subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, name)])
    cert = (x509.CertificateBuilder()
            .subject_name(subject).issuer_name(subject).public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(now() - datetime.timedelta(days=1))
            .not_valid_after(now() + datetime.timedelta(days=365))
            .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
            .add_extension(x509.SubjectKeyIdentifier.from_public_key(key.public_key()), critical=False)
            .add_extension(x509.KeyUsage(digital_signature=True, key_cert_sign=True, crl_sign=True,
                                         content_commitment=False, key_encipherment=False,
                                         data_encipherment=False, key_agreement=False,
                                         encipher_only=False, decipher_only=False), critical=True)
            .sign(key, hashes.SHA256()))
    return key, cert


def make_server_cert(ca_key, ca_cert, ip):
    key = ec.generate_private_key(ec.SECP256R1())
    # mbedTLS matches the host against the CN and DNS names, not IP addresses:
    # the IP goes in all three.
    cert = (x509.CertificateBuilder()
            .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, ip)]))
            .issuer_name(ca_cert.subject).public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(now() - datetime.timedelta(days=1))
            .not_valid_after(now() + datetime.timedelta(days=365))
            .add_extension(x509.SubjectAlternativeName([x509.DNSName(ip),
                                                        x509.IPAddress(ipaddress.ip_address(ip))]),
                           critical=False)
            .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
            .add_extension(x509.SubjectKeyIdentifier.from_public_key(key.public_key()), critical=False)
            .add_extension(x509.AuthorityKeyIdentifier.from_issuer_public_key(ca_key.public_key()),
                           critical=False)
            .sign(ca_key, hashes.SHA256()))
    return key, cert


def spki_der(key):
    return key.public_key().public_bytes(serialization.Encoding.DER,
                                         serialization.PublicFormat.SubjectPublicKeyInfo)


def key_id(key):
    return hashlib.sha256(spki_der(key)).hexdigest()[:8]


def c_string(text):
    return "\n".join('    "' + line + '\\n"' for line in text.strip().splitlines())


def init(args):
    ipaddress.ip_address(args.ip)  # refuse a host name: the certificate is for an address
    BENCH.mkdir(exist_ok=True)
    signing = ec.generate_private_key(ec.SECP256R1())
    ca_key, ca_cert = make_ca("Wignet OTA bench CA")
    server_key, server_cert = make_server_cert(ca_key, ca_cert, args.ip)
    rogue_ca_key, rogue_ca_cert = make_ca("Untrusted CA (bad_tls)")
    rogue_key, rogue_cert = make_server_cert(rogue_ca_key, rogue_ca_cert, args.ip)

    write_private(BENCH / "signing_key.pem", pem(signing))
    write_private(BENCH / "server_key.pem", pem(server_key))
    write_private(BENCH / "rogue_key.pem", pem(rogue_key))
    (BENCH / "server_cert.pem").write_bytes(server_cert.public_bytes(serialization.Encoding.PEM))
    (BENCH / "rogue_cert.pem").write_bytes(rogue_cert.public_bytes(serialization.Encoding.PEM))
    ca_pem = ca_cert.public_bytes(serialization.Encoding.PEM).decode()
    (BENCH / "ca.pem").write_text(ca_pem)

    prefix = f"https://{args.ip}:{args.tls_port}/firmware/"
    (BENCH / "ota_bench.h").write_text(f"""#pragma once

// Generated by `python ota_bench_server.py init` (docs/ota_bench.md): the
// bench's test key {key_id(signing)}, its CA and its server. Not committed.
#define WIGNET_OTA_URL_PREFIX "{prefix}"
#define WIGNET_OTA_NVS "ota_bench"

static const char *const kOtaPublicKeysHex[] = {{
    "{spki_der(signing).hex()}",
    nullptr,
}};

static const char kOtaCaPem[] =
{c_string(ca_pem)}
;
""")
    (BENCH / "bench.json").write_text(json.dumps(
        {"ip": args.ip, "port": args.port, "tls_port": args.tls_port, "counter": 0}, indent=2) + "\n")
    print(f"bench/ ready: test key {key_id(signing)}, server http://{args.ip}:{args.port}, "
          f"firmware {prefix}")
    print("Build and flash the bench image: pio run -e waveshare_photopainter_73e6_ota1_bench -t upload"
          " (or waveshare_driver_75v2_ota1_bench)")


def b64sig(key, lines):
    return base64.b64encode(key.sign("\n".join(lines).encode(), ec.ECDSA(hashes.SHA256()))).decode()


class Bench:
    """One run: one image, one attempt counter, one fault."""

    def __init__(self, args):
        self.settings = json.loads((BENCH / "bench.json").read_text())
        self.signing = serialization.load_pem_private_key((BENCH / "signing_key.pem").read_bytes(), None)
        self.key = key_id(self.signing)
        self.fault = args.fault
        self.image = pathlib.Path(args.image).read_bytes()
        self.sha = hashlib.sha256(self.image).hexdigest()
        last = self.settings["counter"]
        self.counter = args.counter if args.counter is not None else (
            last if self.fault == "wrong_counter" else last + 1)
        if self.counter > last:
            self.settings["counter"] = self.counter
            (BENCH / "bench.json").write_text(json.dumps(self.settings, indent=2) + "\n")
        self.previous_nonce = None
        self.done = None
        self.lock = threading.Lock()

    def offer(self, mac, hw, nonce):
        sha = self.sha
        if self.fault == "wrong_hash":
            sha = hashlib.sha256(self.image + b"bench").hexdigest()
        size = len(self.image) + (16 if self.fault == "wrong_size" else 0)
        hw = OTHER_HW if self.fault == "wrong_hw" else hw
        expires = int(time.time()) + (-3600 if self.fault == "expired" else 86400)
        if self.fault == "stale_nonce":
            nonce = self.previous_nonce or secrets.token_hex(16)
        url = f"https://{self.settings['ip']}:{self.settings['tls_port']}/firmware/{sha}.bin?mac={mac}"
        lines = [OFFER_DOMAIN, mac, hw, sha, str(size), str(self.counter), str(expires), nonce,
                 "powered", "false", url]
        signer = ec.generate_private_key(ec.SECP256R1()) if self.fault == "bad_sig" else self.signing
        return {"id": sha[:16], "url": url, "sha256": sha, "size": size, "hw": hw,
                "counter": self.counter, "expires": expires, "nonce": nonce, "powerSource": "powered",
                "powerOverride": False, "key": self.key, "sig": b64sig(signer, lines)}

    def confirm(self, mac, hw, nonce, running_hash):
        if self.fault == "no_confirm":
            return None
        if self.fault == "replayed_confirm":
            nonce = self.previous_nonce or secrets.token_hex(16)
        lines = [CONFIRM_DOMAIN, mac, hw, running_hash, str(self.counter), nonce]
        signer = ec.generate_private_key(ec.SECP256R1()) if self.fault == "forged_confirm" else self.signing
        return {"sha256": running_hash, "counter": self.counter, "nonce": nonce, "key": self.key,
                "sig": b64sig(signer, lines)}

    def schedule(self, query):
        q = {k: v[0] for k, v in urllib.parse.parse_qs(query).items()}
        mac = q.get("mac", "").lower()
        nonce, report, oc, oh = q.get("on", ""), q.get("ota", ""), q.get("oc", ""), q.get("oh", "")
        answer = {"nextRefreshSec": 15, "currentService": "none", "serverEpoch": int(time.time())}
        with self.lock:
            print(f"{now():%H:%M:%S} poll fw={q.get('fw')} hw={q.get('hw')} on={nonce[:8]} "
                  f"ota={report or '-'} oc={oc or '-'} oh={oh[:16] or '-'}", flush=True)
            # wrong_counter reuses the last run's counter: that run's report is
            # not this one's.
            mine = oc == str(self.counter) and self.fault != "wrong_counter"
            if mine and report.split(":")[0] in ("confirmed", "failed", "rolledback"):
                if self.done != report:
                    self.done = report
                    print(f"  RESULT counter {self.counter}: {report}", flush=True)
            elif report.startswith("pending:") and mine:
                if oh != self.sha:
                    print(f"  pending image {oh[:16]} is not the offered {self.sha[:16]}: no confirmation")
                else:
                    confirm = self.confirm(mac, q.get("hw", ""), nonce, oh)
                    if confirm:
                        answer["otaConfirm"] = confirm
                        print(f"  otaConfirm sent ({self.fault})", flush=True)
            elif "ota1" in q.get("caps", "") and not self.done and len(nonce) == 32:
                answer["otaOffer"] = self.offer(mac, q.get("hw", ""), nonce)
                print(f"  otaOffer {self.sha[:16]} counter {self.counter} ({self.fault})", flush=True)
            self.previous_nonce = nonce or self.previous_nonce
        return answer


class Server(http.server.ThreadingHTTPServer):
    # HTTPServer.server_bind() resolves the bound name (getfqdn), which can
    # hang for long on a machine whose mDNS resolver does not answer.
    def server_bind(self):
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]


def handler(bench, tls):
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args):
            pass

        def send_body(self, code, body, kind):
            self.send_response(code)
            self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path, _, query = self.path.partition("?")
            if not tls and path == "/schedule":
                self.send_body(200, json.dumps(bench.schedule(query)).encode(), "application/json")
            elif not tls and path.startswith("/pic/"):
                # The board keeps the picture it has.
                self.send_response(304)
                self.send_header("Content-Length", "0")
                self.end_headers()
            elif tls and path.startswith("/firmware/") and path.endswith(".bin"):
                print(f"{now():%H:%M:%S} GET {path[:30]}… ({bench.fault})", flush=True)
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(bench.image)))
                self.end_headers()
                body = bench.image[: len(bench.image) // 2] if bench.fault == "truncated" else bench.image
                try:
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError):
                    # The board hangs up as soon as it sees a wrong size.
                    print("  the board closed the download", flush=True)
                    return
                if bench.fault == "truncated":
                    self.close_connection = True
            else:
                self.send_body(404, b"not found\n", "text/plain")

    return Handler


def serve(args):
    bench = Bench(args)
    s = bench.settings
    print(f"Offering {bench.sha[:16]} ({len(bench.image)} bytes) as counter {bench.counter}, "
          f"fault {bench.fault}: {FAULTS[bench.fault]}", flush=True)
    plain = Server(("0.0.0.0", s["port"]), handler(bench, False))
    secure = Server(("0.0.0.0", s["tls_port"]), handler(bench, True))
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    cert, key = ("rogue_cert.pem", "rogue_key.pem") if bench.fault == "bad_tls" else ("server_cert.pem", "server_key.pem")
    context.load_cert_chain(BENCH / cert, BENCH / key)
    secure.socket = context.wrap_socket(secure.socket, server_side=True)
    threading.Thread(target=secure.serve_forever, daemon=True).start()
    print(f"/schedule on http://{s['ip']}:{s['port']}, firmware on https://{s['ip']}:{s['tls_port']}; Ctrl-C to stop",
          flush=True)
    try:
        plain.serve_forever()
    except KeyboardInterrupt:
        print(f"\nStopped. Last result for counter {bench.counter}: {bench.done or 'none'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p_init = sub.add_parser("init", help="generate bench/: test key, CA, certificates, ota_bench.h")
    p_init.add_argument("--ip", required=True, help="this computer's address on the board's network")
    p_init.add_argument("--port", type=int, default=5050, help="HTTP port for /schedule")
    p_init.add_argument("--tls-port", type=int, default=5443, help="HTTPS port for /firmware/")
    p_serve = sub.add_parser("serve", help="offer one image, with one fault")
    p_serve.add_argument("--image", required=True, help="app-only firmware.bin of a bench build")
    p_serve.add_argument("--fault", default="none", choices=sorted(FAULTS),
                         help="; ".join(f"{k}: {v}" for k, v in FAULTS.items()))
    p_serve.add_argument("--counter", type=int, help="attempt counter (default: one above the last)")
    args = parser.parse_args()
    if args.command == "init":
        init(args)
    else:
        if not (BENCH / "bench.json").exists():
            sys.exit("bench/ is missing: run `init` first")
        serve(args)


if __name__ == "__main__":
    main()
