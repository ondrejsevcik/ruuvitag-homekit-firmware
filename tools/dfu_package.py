#!/usr/bin/env python3
"""
Create a Nordic Secure DFU application package (.zip) for the Ruuvi bootloader.

Equivalent to
    nrfutil pkg generate --application app.hex --application-version 1 \
        --hw-version 0xB0 --sd-req 0xB7 --key-file ruuvi_open_private.pem out.zip
(the command in src/targets/ruuvitag_b/armgcc/package.sh) without depending on the
legacy Python 2 era nrfutil, which fails to sign on current Python versions.

The init packet is the nRF5 SDK 15.3 dfu-cc protobuf, signed with ECDSA P-256 /
SHA-256 over the serialized InitCommand, signature r || s each little-endian, which
is what nrfutil produces and the SDK 15.3 bootloader verifies.

Requires: python3 -m pip install cryptography
"""

import argparse
import hashlib
import io
import json
import sys
import zipfile

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature

# dfu-cc.proto enums.
OPCODE_INIT = 1
FWTYPE_APPLICATION = 0
HASHTYPE_SHA256 = 3
VALIDATE_GENERATED_CRC = 1
SIGNATURE_ECDSA_P256_SHA256 = 0


# ---------------------------------------------------------------------------------------------------------------------
# Minimal protobuf encoding.

def varint(value: int) -> bytes:
    out = bytearray()
    while True:
        b = value & 0x7F
        value >>= 7
        if value:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def field_varint(number: int, value: int) -> bytes:
    return varint(number << 3) + varint(value)


def field_bytes(number: int, value: bytes) -> bytes:
    return varint((number << 3) | 2) + varint(len(value)) + value


def field_packed(number: int, values: list) -> bytes:
    return field_bytes(number, b"".join(varint(v) for v in values))


# ---------------------------------------------------------------------------------------------------------------------
# Intel HEX.

def read_hex(path: str) -> tuple:
    memory = {}
    base = 0
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line.startswith(":"):
                continue
            record = bytes.fromhex(line[1:])
            if sum(record) & 0xFF:
                raise ValueError(f"checksum error in {path}: {line}")
            length, address, kind, data = record[0], (record[1] << 8) | record[2], record[3], record[4 : 4 + record[0]]
            if kind == 0:
                for i, b in enumerate(data):
                    memory[base + address + i] = b
            elif kind == 2:
                base = int.from_bytes(data, "big") << 4
            elif kind == 4:
                base = int.from_bytes(data, "big") << 16
            elif kind == 1:
                break
    start, end = min(memory), max(memory) + 1
    image = bytes(memory.get(a, 0xFF) for a in range(start, end))
    return start, image


# ---------------------------------------------------------------------------------------------------------------------

def init_command(image: bytes, fw_version: int, hw_version: int, sd_req: list) -> bytes:
    firmware_hash = hashlib.sha256(image).digest()[::-1]  # nrfutil stores the digest little-endian
    hash_message = field_varint(1, HASHTYPE_SHA256) + field_bytes(2, firmware_hash)
    boot_validation = field_varint(1, VALIDATE_GENERATED_CRC) + field_bytes(2, b"")
    return (
        field_varint(1, fw_version)
        + field_varint(2, hw_version)
        + field_packed(3, sd_req)
        + field_varint(4, FWTYPE_APPLICATION)
        + field_varint(5, 0)
        + field_varint(6, 0)
        + field_varint(7, len(image))
        + field_bytes(8, hash_message)
        + field_varint(9, 0)
        + field_bytes(10, boot_validation)
    )


def sign(key_path: str, data: bytes) -> bytes:
    with open(key_path, "rb") as f:
        key = serialization.load_pem_private_key(f.read(), password=None)
    if not isinstance(key, ec.EllipticCurvePrivateKey) or key.curve.name != "secp256r1":
        sys.exit(f"{key_path} is not a P-256 private key")
    r, s = decode_dss_signature(key.sign(data, ec.ECDSA(hashes.SHA256())))
    return r.to_bytes(32, "big")[::-1] + s.to_bytes(32, "big")[::-1]


def build(app_hex: str, key: str, out: str, fw_version: int, hw_version: int, sd_req: list) -> None:
    start, image = read_hex(app_hex)
    command = init_command(image, fw_version, hw_version, sd_req)
    signature = sign(key, command)
    signed = (
        field_bytes(1, field_varint(1, OPCODE_INIT) + field_bytes(2, command))
        + field_varint(2, SIGNATURE_ECDSA_P256_SHA256)
        + field_bytes(3, signature)
    )
    packet = field_bytes(2, signed)

    manifest = {"manifest": {"application": {"bin_file": "application.bin", "dat_file": "application.dat"}}}
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("application.bin", image)
        z.writestr("application.dat", packet)
        z.writestr("manifest.json", json.dumps(manifest, indent=4))
    print(f"{out}: application 0x{start:05X}..0x{start + len(image):05X} ({len(image)} bytes), "
          f"fw_version {fw_version}, hw_version 0x{hw_version:X}, sd_req {[hex(v) for v in sd_req]}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("application", help="application .hex")
    parser.add_argument("output", help="output .zip")
    parser.add_argument("--key-file", required=True, help="PEM signing key (e.g. ruuvi_open_private.pem)")
    parser.add_argument("--application-version", type=int, default=1)
    parser.add_argument("--hw-version", type=lambda v: int(v, 0), default=0xB0)
    parser.add_argument("--sd-req", type=lambda v: [int(x, 0) for x in v.split(",")], default=[0xB7])
    args = parser.parse_args()
    build(args.application, args.key_file, args.output, args.application_version, args.hw_version, args.sd_req)


if __name__ == "__main__":
    main()
