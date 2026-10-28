#!/usr/bin/env python3
"""Real loopback HTTPS + signed JSON + ZIP integration smoke.

Runs the production WindowsUpdateController, ZIP installer and RSA verifier
against an ephemeral TLS service. No private release keys are used.
"""
import argparse
import hashlib
import http.server
import io
import json
import os
from pathlib import Path
import shutil
import ssl
import subprocess
import tempfile
import threading
import zipfile


IDENTITY = "nightly-20261029-012345-a743adbc1dfb"


def run(*args):
    subprocess.run(list(map(str, args)), check=True, capture_output=True, text=True)


def fixture_zip():
    buffer = io.BytesIO()
    base = f"MatterLeast/versions/{IDENTITY}/MatterLeast/bin/"
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("MatterLeast/MatterLeastLauncher.exe", b"launcher")
        z.writestr(base + "matterleast.exe", b"test exe")
        z.writestr(base + "MatterLeastUpdater.exe", b"test updater")
        # Enough incompressible bytes for the 1024-byte minimum manifest size.
        z.writestr(base + "Qt6Core.dll", os.urandom(8192))
    return buffer.getvalue()


def write_fixtures(folder, port, signing_key):
    raw_zip = fixture_zip()
    payloads = {}
    for scenario in ("valid", "tampered", "bad-signature", "missing-signature", "bad-zip"):
        manifest = {
            "schema": 1, "platform": "windows-x64", "channel": "nightly",
            "version": "2.0", "releaseTag": "updates-nightly",
            "buildId": IDENTITY, "minimumLauncherProtocol": 1,
            "package": {"format": "portable-zip",
                        "url": f"https://127.0.0.1:{port}/{scenario}/package.zip",
                        "sha256": hashlib.sha256(raw_zip).hexdigest(),
                        "size": len(raw_zip)},
        }
        raw = json.dumps(manifest, separators=(",", ":")).encode("utf-8")
        json_path = folder / (scenario + ".json")
        json_path.write_bytes(raw)
        signature_path = folder / (scenario + ".sig")
        run("openssl", "dgst", "-sha256", "-sign", signing_key,
            "-out", signature_path, json_path)
        sig = signature_path.read_bytes()
        if scenario == "tampered":
            # Byte-level post-signing modification, while JSON stays valid.
            raw = raw.replace(b'"version":"2.0"', b'"version":"3.0"')
        elif scenario == "bad-signature":
            sig = bytes([sig[0] ^ 1]) + sig[1:]
        payloads[f"/{scenario}/windows-x64.json"] = raw
        if scenario != "missing-signature":
            payloads[f"/{scenario}/windows-x64.json.sig"] = sig
        payloads[f"/{scenario}/package.zip"] = (
            raw_zip if scenario != "bad-zip" else raw_zip[:-1] + bytes([raw_zip[-1] ^ 1]))
    return payloads


def test_case(root, scenario, expected, probe, cert, pubkey, port, hits):
    install = root / ("install-" + scenario)
    binary = install / "versions" / "v1.0" / "MatterLeast" / "bin"
    binary.mkdir(parents=True)
    shutil.copy2(probe, binary / "matterleast.exe")
    (install / "MatterLeastLauncher.exe").write_bytes(b"stub")
    (install / "current.version").write_text("v1.0", encoding="utf-8")
    env = dict(os.environ,
               MATTERLEAST_SIGNED_SMOKE_URL=(
                   f"https://127.0.0.1:{port}/{scenario}/windows-x64.json"),
               MATTERLEAST_SIGNED_SMOKE_PUBLIC_KEY=str(pubkey),
               MATTERLEAST_SIGNED_SMOKE_TLS_CERT=str(cert),
               MATTERLEAST_SIGNED_SMOKE_EXPECT=expected)
    output = subprocess.run([str(binary / "matterleast.exe")], env=env,
                            capture_output=True, text=True, timeout=100)
    print(f"{scenario}: exit={output.returncode}\n{output.stdout}\n{output.stderr}", flush=True)
    if output.returncode:
        raise AssertionError(f"Signed test {scenario} failed (exit {output.returncode})")
    package_path = f"/{scenario}/package.zip"
    expected_hits = 1 if scenario in ("valid", "bad-zip") else 0
    if hits.count(package_path) != expected_hits:
        raise AssertionError(
            f"{scenario}: expected {expected_hits} ZIP request(s), got {hits.count(package_path)}")
    if scenario == "valid" and not (
        install / "versions" / IDENTITY / "MatterLeast" / "bin" / "matterleast.exe"
    ).is_file():
        raise AssertionError("A signed and verified ZIP wasn't prepared")
    print(f"PASS {scenario}: ZIP GETs={expected_hits}", flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="matterleast-signed-") as temp:
        root = Path(temp)
        signing_key = root / "signing-private.pem"
        public_key = root / "signing-public.pem"
        cert_key = root / "tls-key.pem"
        cert = root / "tls-cert.pem"
        run("openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt",
            "rsa_keygen_bits:3072", "-out", signing_key)
        run("openssl", "pkey", "-in", signing_key, "-pubout", "-out", public_key)
        run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", cert_key, "-out", cert, "-days", "1", "-subj", "/CN=127.0.0.1",
            "-addext", "subjectAltName=IP:127.0.0.1")
        hits = []
        payloads = {}

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                hits.append(self.path)
                body = payloads.get(self.path)
                if body is None:
                    self.send_error(404, "fixture intentionally missing")
                    return
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, fmt, *items):
                print("HTTPS:", fmt % items, flush=True)

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        port = server.server_port
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(certfile=cert, keyfile=cert_key)
        server.socket = ctx.wrap_socket(server.socket, server_side=True)
        payloads.update(write_fixtures(root, port, signing_key))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            for scenario, expected in (
                ("valid", "ready"), ("tampered", "signature"),
                ("bad-signature", "signature"),
                ("missing-signature", "metadata"),
                ("bad-zip", "sha"),
            ):
                test_case(root, scenario, expected, args.probe.resolve(),
                          cert, public_key, port, hits)
        finally:
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    main()
