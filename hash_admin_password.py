#!/usr/bin/env python3
import base64
import hashlib
import os
import sys


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).decode("ascii").rstrip("=")


def main() -> int:
    username = sys.argv[1] if len(sys.argv) > 1 else "admin"
    password = sys.argv[2] if len(sys.argv) > 2 else "admin"
    iterations = int(sys.argv[3]) if len(sys.argv) > 3 else 100000
    salt = os.urandom(16)
    digest = hashlib.pbkdf2_hmac(
        "sha256", password.encode("utf-8"), salt, iterations, 32)
    print(
        f"{username} = pbkdf2_sha256${iterations}"
        f"${b64url(salt)}${b64url(digest)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
