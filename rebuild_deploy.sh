#!/bin/bash
# ============================================================
# asio_owen 固定 Admin 凭证 + 完整重建 + 测试 + 部署 + 冒烟测试
# 用法: bash rebuild_deploy.sh
# 可选：RUN_TESTS=0 bash rebuild_deploy.sh 跳过 ctest。
# 可选：ADMIN_SECRET_DIR=/安全持久目录 ADMIN_USERNAME=ops bash rebuild_deploy.sh
# ============================================================
set -euo pipefail

ROOT=${ROOT:-/mnt/mac/Users/mac/code/croot/asio_owen}
BUILD_DIR=${BUILD_DIR:-$ROOT/build_ubuntu}
CANDIDATE_BUILD_DIR="${BUILD_DIR}.candidate"
BACKUP_BUILD_DIR="${BUILD_DIR}.previous_$(date -u +%Y%m%dT%H%M%SZ)"
SERVICE=asio-owen.service
RUN_TESTS=${RUN_TESTS:-1}
KEEP_DEBUG_SYMBOLS=${KEEP_DEBUG_SYMBOLS:-1}
ADMIN_SECRET_DIR=${ADMIN_SECRET_DIR:-/etc/asio-owen/admin}
ADMIN_USERNAME=${ADMIN_USERNAME:-admin}
ADMIN_PRIVATE_KEY="$ADMIN_SECRET_DIR/admin-private-key.pem"
ADMIN_PUBLIC_KEY="$ADMIN_SECRET_DIR/admin-public-key.pem"
ADMIN_ACCOUNT_FILE="$ADMIN_SECRET_DIR/admin-account.ini"
LOG_DIR="$ROOT/logs"
LOG_FILE="$LOG_DIR/rebuild_deploy_$(date -u +%Y%m%dT%H%M%SZ).log"

mkdir -p "$LOG_DIR"
exec > >(tee -a "$LOG_FILE") 2>&1

trap 'echo "=== FAILED at line $LINENO; log: $LOG_FILE ==="' ERR
cd "$ROOT"

echo "=== rebuild_deploy started: $(date -u +%FT%TZ) ==="
echo "=== log: $LOG_FILE ==="

prepare_admin_credentials() {
    if [[ ! "$ADMIN_USERNAME" =~ ^[A-Za-z0-9_.-]{1,64}$ ]]; then
        echo "ERROR: ADMIN_USERNAME must match [A-Za-z0-9_.-]{1,64}"
        return 1
    fi
    if [[ "$ADMIN_SECRET_DIR" != /* || "$ADMIN_SECRET_DIR" == *$'\n'* ||
          "$ADMIN_SECRET_DIR" == *$'\r'* ]]; then
        echo "ERROR: ADMIN_SECRET_DIR must be an absolute path without newlines"
        return 1
    fi
    local normalized_secret_dir
    local normalized_build_dir
    local normalized_candidate_dir
    normalized_secret_dir=$(realpath -m -- "$ADMIN_SECRET_DIR")
    normalized_build_dir=$(realpath -m -- "$BUILD_DIR")
    normalized_candidate_dir=$(realpath -m -- "$CANDIDATE_BUILD_DIR")
    case "${normalized_secret_dir}/" in
        "${normalized_build_dir}/"*|"${normalized_candidate_dir}/"*)
            echo "ERROR: ADMIN_SECRET_DIR must be outside BUILD_DIR and its candidate directory"
            return 1
            ;;
    esac

    umask 077
    mkdir -p "$ADMIN_SECRET_DIR"
    chmod 700 "$ADMIN_SECRET_DIR"

    if [[ ! -e "$ADMIN_PRIVATE_KEY" && ! -e "$ADMIN_PUBLIC_KEY" ]]; then
        echo "No persistent Admin key pair found; generating it once in $ADMIN_SECRET_DIR"
        "$ROOT/gen_admin_keys.sh" "$ADMIN_SECRET_DIR"
    elif [[ ! -f "$ADMIN_PRIVATE_KEY" || ! -f "$ADMIN_PUBLIC_KEY" ]]; then
        echo "ERROR: incomplete Admin key pair in $ADMIN_SECRET_DIR; refusing to rotate one side"
        return 1
    fi

    local private_fingerprint
    local public_fingerprint
    if ! private_fingerprint=$(openssl pkey -in "$ADMIN_PRIVATE_KEY" -pubout \
        -outform DER 2>/dev/null | openssl dgst -sha256); then
        echo "ERROR: invalid Admin private key: $ADMIN_PRIVATE_KEY"
        return 1
    fi
    if ! public_fingerprint=$(openssl pkey -pubin -in "$ADMIN_PUBLIC_KEY" \
        -outform DER 2>/dev/null | openssl dgst -sha256); then
        echo "ERROR: invalid Admin public key: $ADMIN_PUBLIC_KEY"
        return 1
    fi
    if [[ "$private_fingerprint" != "$public_fingerprint" ]]; then
        echo "ERROR: Admin private/public keys do not match"
        return 1
    fi
    chmod 600 "$ADMIN_PRIVATE_KEY"
    chmod 644 "$ADMIN_PUBLIC_KEY"

    if [[ ! -f "$ADMIN_ACCOUNT_FILE" ]]; then
        echo "No persistent Admin account found; set the password for '$ADMIN_USERNAME'."
        local account_line
        account_line=$("$ROOT/hash_admin_password.py" "$ADMIN_USERNAME")
        local account_tmp
        account_tmp=$(mktemp "$ADMIN_SECRET_DIR/.admin-account.XXXXXX")
        printf '[admin]\n%s\n' "$account_line" > "$account_tmp"
        chmod 600 "$account_tmp"
        mv "$account_tmp" "$ADMIN_ACCOUNT_FILE"
        unset account_line
    fi

    if ! grep -Eq "^${ADMIN_USERNAME}[[:space:]]*=[[:space:]]*pbkdf2_sha256\\\$[0-9]+\\\$[A-Za-z0-9_-]+\\\$[A-Za-z0-9_-]+[[:space:]]*$" \
        "$ADMIN_ACCOUNT_FILE"; then
        echo "ERROR: $ADMIN_ACCOUNT_FILE does not contain the fixed '$ADMIN_USERNAME' account"
        return 1
    fi
    chmod 600 "$ADMIN_ACCOUNT_FILE"
}

install_admin_credentials() {
    local candidate_local="$CANDIDATE_BUILD_DIR/config.d/99-local.ini"
    install -m 600 "$ADMIN_ACCOUNT_FILE" "$candidate_local"
    printf '\n[admin]\njwt_private_key = %s\njwt_public_key = %s\n' \
        "$ADMIN_PRIVATE_KEY" "$ADMIN_PUBLIC_KEY" >> "$candidate_local"
}

echo "=== [1/9] 准备固定 Admin 密钥和账号 ==="
prepare_admin_credentials
echo "Admin credentials: $ADMIN_SECRET_DIR (username=$ADMIN_USERNAME)"

echo "=== [2/9] 清理候选构建 ==="
rm -rf "$CANDIDATE_BUILD_DIR"

echo "=== [3/9] cmake 配置候选构建 (Release + tests) ==="
/usr/bin/cmake -B "$CANDIDATE_BUILD_DIR" -S . -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON

echo "=== [4/9] 注入固定 Admin 凭证 ==="
install_admin_credentials

echo "=== [5/9] 编译候选 server 和测试 ==="
/usr/bin/cmake --build "$CANDIDATE_BUILD_DIR" -j2

if [[ "$RUN_TESTS" == "1" ]]; then
    echo "=== [6/9] ctest ==="
    ctest --test-dir "$CANDIDATE_BUILD_DIR" --output-on-failure
else
    echo "=== [6/9] ctest skipped (RUN_TESTS=$RUN_TESTS) ==="
fi

echo "=== [7/9] strip 候选二进制 ==="
if [[ "$KEEP_DEBUG_SYMBOLS" == "1" ]]; then
    objcopy --only-keep-debug "$CANDIDATE_BUILD_DIR/server" "$CANDIDATE_BUILD_DIR/server.debug"
    strip --strip-unneeded "$CANDIDATE_BUILD_DIR/server"
    objcopy --add-gnu-debuglink="$CANDIDATE_BUILD_DIR/server.debug" "$CANDIDATE_BUILD_DIR/server"
    echo "debug symbols: $CANDIDATE_BUILD_DIR/server.debug"
else
    strip --strip-unneeded "$CANDIDATE_BUILD_DIR/server"
fi
ls -lh "$CANDIDATE_BUILD_DIR/server"

echo "=== [8/9] 切换构建并重启服务 ==="
sudo systemctl stop "$SERVICE"
sudo systemctl stop asio-owen-asan.service 2>/dev/null || true
if [[ -e "$BUILD_DIR" ]]; then
    mv "$BUILD_DIR" "$BACKUP_BUILD_DIR"
fi
mv "$CANDIDATE_BUILD_DIR" "$BUILD_DIR"
sudo dmesg -c > /dev/null 2>&1 || true
sudo systemctl restart "$SERVICE"
sleep 5

smoke() {
    local name=$1
    shift
    local body
    body=$(mktemp)
    local status
    status=$(curl --connect-timeout 2 --max-time 10 -sS -o "$body" -w "%{http_code}" "$@")
    echo "$name: $status"
    if [[ "$status" != "200" ]]; then
        echo "--- $name response ---"
        cat "$body"
        rm -f "$body"
        return 1
    fi
    rm -f "$body"
}

echo "=== [9/9] 冒烟测试 ==="
smoke "Health" http://127.0.0.1:8081/api/health
smoke "MySQL" http://127.0.0.1:8081/api/mysql
smoke "Redis" http://127.0.0.1:8081/api/redis
smoke "Gateway" http://127.0.0.1:8081/zebra-config/config.ConfigService/GetByAppAndKey \
    -X POST -H "Content-Type: application/json" -d "{}"

echo ""
echo "=== segfault: $(sudo dmesg | grep -c segfault || true) ==="
echo "=== 完成: $(date -u +%FT%TZ) ==="
echo "=== log: $LOG_FILE ==="
echo "=== previous build: ${BACKUP_BUILD_DIR:-none} ==="
