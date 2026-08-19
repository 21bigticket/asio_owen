#!/usr/bin/env python3
import base64
import getpass
import hashlib
import os
import sys


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).decode("ascii").rstrip("=")


def main() -> int:
    if len(sys.argv) not in (2, 3):
        print(
            f"usage: {sys.argv[0]} <username> [iterations]",
            file=sys.stderr,
        )
        return 2

    username = sys.argv[1]
    if not username:
        print("username must not be empty", file=sys.stderr)
        return 2

    try:
        iterations = int(sys.argv[2]) if len(sys.argv) == 3 else 100000
    except ValueError:
        print("iterations must be an integer", file=sys.stderr)
        return 2
    if iterations < 100000:
        print("iterations must be at least 100000", file=sys.stderr)
        return 2

    password = getpass.getpass("Password: ")
    confirmation = getpass.getpass("Confirm password: ")
    if not password:
        print("password must not be empty", file=sys.stderr)
        return 2
    if password != confirmation:
        print("passwords do not match", file=sys.stderr)
        return 2

    salt = os.urandom(16)
    digest = hashlib.pbkdf2_hmac(
        "sha256", password.encode("utf-8"), salt, iterations, 32)
    print(
        f"{username} = pbkdf2_sha256${iterations}"
        f"${b64url(salt)}${b64url(digest)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
