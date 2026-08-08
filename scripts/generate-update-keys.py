#!/usr/bin/env python3
"""Generate an ed25519 keypair for GWDash update manifests."""

from __future__ import annotations

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives import serialization


def main() -> None:
    sk = Ed25519PrivateKey.generate()
    pk = sk.public_key()
    raw_sk = sk.private_bytes(
        encoding=serialization.Encoding.Raw,
        format=serialization.PrivateFormat.Raw,
        encryption_algorithm=serialization.NoEncryption(),
    )
    raw_pk = pk.public_bytes(
        encoding=serialization.Encoding.Raw,
        format=serialization.PublicFormat.Raw,
    )

    print("Private seed (store as GitHub Actions secret GWDASH_UPDATE_ED25519_SK):")
    print(raw_sk.hex())
    print()
    print("Public key bytes for src/UpdatePublicKey.h:")
    chunks = [f"0x{b:02x}" for b in raw_pk]
    lines = []
    for i in range(0, len(chunks), 16):
        lines.append("        " + ", ".join(chunks[i : i + 16]) + ",")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
