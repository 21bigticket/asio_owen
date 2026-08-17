#!/usr/bin/env bash
set -euo pipefail

dir="${1:-jwt_keys}"
mkdir -p "$dir"
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 \
  -out "$dir/admin-private-key.pem"
chmod 600 "$dir/admin-private-key.pem"
openssl rsa -pubout \
  -in "$dir/admin-private-key.pem" \
  -out "$dir/admin-public-key.pem"
echo "generated $dir/admin-private-key.pem and $dir/admin-public-key.pem"
echo "run cmake --build build --target server to refresh build/jwt_keys"
