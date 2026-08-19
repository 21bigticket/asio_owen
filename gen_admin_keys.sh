#!/usr/bin/env bash
set -euo pipefail

dir="${1:-build/jwt_keys}"
mkdir -p "$dir"
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 \
  -out "$dir/admin-private-key.pem"
chmod 600 "$dir/admin-private-key.pem"
openssl rsa -pubout \
  -in "$dir/admin-private-key.pem" \
  -out "$dir/admin-public-key.pem"
echo "generated $dir/admin-private-key.pem and $dir/admin-public-key.pem"
echo "keep the private key in this runtime directory; the build does not copy it"
