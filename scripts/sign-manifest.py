#!/usr/bin/env python3
"""Build and sign dist/manifest.json + dist/manifest.sig for a release."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import sys

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey


def sha256_file(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def signing_bytes(manifest: dict) -> bytes:
    # Must match ManifestSigningBytes in src/Manifest.cpp.
    parts = [f'{{"v":{manifest["v"]},"version":"{manifest["version"]}","files":[']
    files = []
    for entry in manifest["files"]:
        files.append(f'{{"name":"{entry["name"]}","sha256":"{entry["sha256"]}"}}')
    parts.append(",".join(files))
    parts.append("]}")
    return "".join(parts).encode("utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dist", type=pathlib.Path, default=pathlib.Path("dist"))
    parser.add_argument("--version", required=True)
    args = parser.parse_args()

    sk_hex = os.environ.get("GWDASH_UPDATE_ED25519_SK", "").strip()
    if len(sk_hex) != 64:
        print("GWDASH_UPDATE_ED25519_SK must be a 64-char hex ed25519 seed", file=sys.stderr)
        return 1

    files = []
    for name in ("GWDash.dll", "GWDash.core.dll"):
        path = args.dist / name
        if not path.is_file():
            print(f"missing {path}", file=sys.stderr)
            return 1
        files.append({"name": name, "sha256": sha256_file(path)})

    manifest = {"v": 1, "version": args.version, "files": files}
    payload = signing_bytes(manifest)

    sk = Ed25519PrivateKey.from_private_bytes(bytes.fromhex(sk_hex))
    signature = sk.sign(payload)

    (args.dist / "manifest.json").write_text(json.dumps(manifest, separators=(",", ":")), encoding="utf-8")
    (args.dist / "manifest.sig").write_text(signature.hex() + "\n", encoding="ascii")
    print("Wrote manifest.json and manifest.sig")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
