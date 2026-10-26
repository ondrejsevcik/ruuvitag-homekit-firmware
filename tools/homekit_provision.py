#!/usr/bin/env python3
"""
Provision HomeKit setup information for the RuuviTag HomeKit firmware.

HomeKit pairing uses SRP-6a with an 8 digit setup code. The accessory never
needs the code itself, only a random salt and the SRP verifier derived from
the code. This tool keeps those in a JSON file (default:
``homekit-setup.json`` in the repository root, git-ignored) and renders a C
header that is compiled into the firmware.

The same JSON is reused on every build so the setup code stays stable across
firmware updates. Delete it (or pass --new) to roll a new code; already paired
controllers keep working because pairing keys are stored on the tag.

Usage:
    tools/homekit_provision.py header OUT.h        # create/reuse setup, write header
    tools/homekit_provision.py show                # print code, setup payload and QR
    tools/homekit_provision.py header OUT.h --code 123-45-670 --new

Only the Python standard library is required. If ``qrencode`` is installed a
scannable QR code is printed / written next to the JSON as SVG.
"""

import argparse
import hashlib
import json
import os
import secrets
import shutil
import subprocess
import sys

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DEFAULT_STATE = os.path.join(REPO_ROOT, "homekit-setup.json")

# HomeKit accessory category "Sensor" (HAP spec table 12-3).
CATEGORY_SENSORS = 10

# RFC 5054 3072-bit group, generator 5, as mandated by HAP Pair Setup.
SRP_N = int(
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74"
    "020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F1437"
    "4FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF05"
    "98DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F356208552BB"
    "9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
    "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF695581718"
    "3995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33"
    "A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7"
    "ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864"
    "D87602733EC86A64521F2B18177B200CBBE117577A615D6C770988C0BAD946E2"
    "08E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF",
    16,
)
SRP_G = 5
SRP_USERNAME = b"Pair-Setup"
SRP_PRIME_BYTES = 384


def is_valid_setup_code(code: str) -> bool:
    """Mirror of HAPAccessorySetupIsValidSetupCode()."""
    if len(code) != 10 or code[3] != "-" or code[6] != "-":
        return False
    digits = code[:3] + code[4:6] + code[7:]
    if not digits.isdigit():
        return False
    pairs = list(zip(digits, digits[1:]))
    if all(a == b for a, b in pairs):
        return False
    if digits == "12345678" or digits == "87654321":
        return False
    return True


def random_setup_code() -> str:
    while True:
        d = "".join(str(secrets.randbelow(10)) for _ in range(8))
        code = f"{d[:3]}-{d[3:5]}-{d[5:]}"
        if is_valid_setup_code(code):
            return code


def random_setup_id() -> str:
    alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    return "".join(secrets.choice(alphabet) for _ in range(4))


def srp_verifier(salt: bytes, code: str) -> bytes:
    inner = hashlib.sha512(SRP_USERNAME + b":" + code.encode()).digest()
    x = int.from_bytes(hashlib.sha512(salt + inner).digest(), "big")
    v = pow(SRP_G, x, SRP_N)
    return v.to_bytes(SRP_PRIME_BYTES, "big")


def setup_payload(code: str, setup_id: str, category: int = CATEGORY_SENSORS,
                  ble: bool = True, ip: bool = False, paired: bool = False) -> str:
    """Mirror of HAPAccessorySetupGetSetupPayload()."""
    value = ((category & 0xFF) << 31) | (int(ble) << 29) | (int(ip) << 28) | (int(paired) << 27)
    value |= int(code.replace("-", ""))
    alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    encoded = ""
    for _ in range(9):
        value, r = divmod(value, 36)
        encoded = alphabet[r] + encoded
    return "X-HM://" + encoded + setup_id


def load_or_create(path: str, code: str = None, setup_id: str = None, new: bool = False) -> dict:
    if os.path.exists(path) and not new:
        with open(path) as f:
            state = json.load(f)
        if code and code != state["setup_code"]:
            sys.exit(f"{path} already holds setup code {state['setup_code']}; pass --new to replace it.")
        return state

    code = code or random_setup_code()
    if not is_valid_setup_code(code):
        sys.exit(f"Invalid setup code {code!r}: use XXX-XX-XXX and avoid trivial codes.")
    setup_id = (setup_id or random_setup_id()).upper()
    if len(setup_id) != 4 or not all(c.isdigit() or "A" <= c <= "Z" for c in setup_id):
        sys.exit(f"Invalid setup ID {setup_id!r}: use 4 characters 0-9 / A-Z.")
    salt = secrets.token_bytes(16)
    state = {
        "setup_code": code,
        "setup_id": setup_id,
        "salt": salt.hex(),
        "verifier": srp_verifier(salt, code).hex(),
        "setup_payload": setup_payload(code, setup_id),
    }
    old_umask = os.umask(0o077)
    try:
        with open(path, "w") as f:
            json.dump(state, f, indent=2)
            f.write("\n")
    finally:
        os.umask(old_umask)
    print(f"Created new HomeKit setup info in {path}", file=sys.stderr)
    return state


def c_array(data: bytes, indent: str = "    ") -> str:
    lines = []
    for i in range(0, len(data), 12):
        lines.append(indent + ", ".join(f"0x{b:02X}" for b in data[i:i + 12]) + ",")
    return "\n".join(lines)


def write_header(state: dict, out: str) -> None:
    salt = bytes.fromhex(state["salt"])
    verifier = bytes.fromhex(state["verifier"])
    assert verifier == srp_verifier(salt, state["setup_code"]), "verifier does not match setup code"
    text = f"""/* Generated by tools/homekit_provision.py - do not edit, do not commit. */
#ifndef HOMEKIT_SETUP_INFO_H
#define HOMEKIT_SETUP_INFO_H

#include <stdint.h>

/** Setup ID used in the setup payload (QR / NFC) and the advertised setup hash. */
#define HOMEKIT_SETUP_ID "{state['setup_id']}"

/** Setup payload, for reference. Encodes the setup code, so treat it as secret. */
#define HOMEKIT_SETUP_PAYLOAD "{state['setup_payload']}"

/** SRP salt for the setup code. */
#define HOMEKIT_SETUP_SALT {{ \\
{c_array(salt, "    ").replace(chr(10), " " + chr(92) + chr(10))} \\
}}

/** SRP verifier for the setup code. */
#define HOMEKIT_SETUP_VERIFIER {{ \\
{c_array(verifier, "    ").replace(chr(10), " " + chr(92) + chr(10))} \\
}}

#endif
"""
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    tmp = out + ".tmp"
    with open(tmp, "w") as f:
        f.write(text)
    os.replace(tmp, out)


def show(state: dict, state_path: str) -> None:
    print(f"HomeKit setup code : {state['setup_code']}")
    print(f"Setup ID           : {state['setup_id']}")
    print(f"Setup payload      : {state['setup_payload']}")
    if shutil.which("qrencode"):
        svg = os.path.splitext(state_path)[0] + "-qr.svg"
        subprocess.run(["qrencode", "-t", "SVG", "-l", "Q", "-m", "2", "-o", svg, state["setup_payload"]], check=True)
        print(f"QR code (SVG)      : {svg}")
        subprocess.run(["qrencode", "-t", "UTF8", "-l", "Q", "-m", "2", state["setup_payload"]], check=True)
    else:
        print("Install 'qrencode' to render a scannable QR code, or type the setup code manually.")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=["header", "show"])
    parser.add_argument("output", nargs="?", help="header path (for 'header')")
    parser.add_argument("--state", default=DEFAULT_STATE, help="setup info JSON (default: %(default)s)")
    parser.add_argument("--code", help="setup code XXX-XX-XXX (default: random)")
    parser.add_argument("--setup-id", help="4 character setup ID (default: random)")
    parser.add_argument("--new", action="store_true", help="discard existing setup info and create new")
    args = parser.parse_args()

    state = load_or_create(args.state, args.code, args.setup_id, args.new)
    if args.command == "header":
        if not args.output:
            parser.error("header needs an output path")
        write_header(state, args.output)
    else:
        show(state, args.state)


if __name__ == "__main__":
    main()
