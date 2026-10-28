#!/usr/bin/env python3
"""Create the canonical manifest consumed by the Digipet OTA updater."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--firmware", required=True, type=Path)
    parser.add_argument("--factory", type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    firmware = args.firmware.read_bytes()
    version = args.version.removeprefix("v")
    release_base = (
        f"https://github.com/{args.repository}/releases/download/v{version}"
    )
    manifest: dict[str, object] = {
        "schema": 1,
        "product": "digipet",
        "target": "waveshare-esp32-s3-touch-amoled-2.16",
        "version": version,
        "firmware": {
            "filename": "digipet-firmware.bin",
            "url": f"{release_base}/digipet-firmware.bin",
            "size": len(firmware),
            "sha256": hashlib.sha256(firmware).hexdigest(),
        },
        "signature": {
            "algorithm": "ECDSA-P256-SHA256",
            "filename": "digipet-manifest.sig",
            "url": f"{release_base}/digipet-manifest.sig",
        },
    }
    if args.factory:
        factory = args.factory.read_bytes()
        manifest["factory"] = {
            "filename": "digipet-factory.bin",
            "url": f"{release_base}/digipet-factory.bin",
            "size": len(factory),
            "sha256": hashlib.sha256(factory).hexdigest(),
        }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


if __name__ == "__main__":
    main()
