#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPOSITORY_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
WORKSPACE_ROOT="$(cd "${REPOSITORY_ROOT}/.." && pwd)"
BUILD_DIR="${WORKSPACE_ROOT}/build"
DEFAULT_OUTPUT_DIR="${TMPDIR:-/tmp}/amatsukaze_update_release"
ARCHIVE_PREFIX="Amatsukaze_Ubuntu24.04_"
PROGRESS_INTERVAL_SECONDS=5
INCLUDED_DIRECTORIES=(exe_files JL avs bat profile scripts)
INCLUDED_FILES=(AmatsukazeServer.sh)
# 展開側はパス要素にコロンを含むエントリを危険と判定して展開全体を失敗させる。
# WSL上のビルドには「foo.py:Zone.Identifier」のようなNTFS副ストリームの痕跡が混ざるため、
# 本物のリリースには存在しないこれらをアーカイブから除外する。
EXCLUDED_NAME_PATTERN='*:*'
REQUIRED_COMMANDS=(tar xz stat find)
COMPRESS_PID=""

usage() {
    cat <<'EOF'
使い方: make_release.sh <version> [出力先ディレクトリ]
例:     make_release.sh 9.9.9 /tmp/amt_release
EOF
}

if [[ $# -lt 1 || $# -gt 2 ]]; then
    usage >&2
    exit 1
fi

for command_name in "${REQUIRED_COMMANDS[@]}"; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        echo "[make_release] 必要なコマンドが見つからない: ${command_name}" >&2
        exit 1
    fi
done

VERSION="$1"
OUTPUT_DIR="${2:-${DEFAULT_OUTPUT_DIR}}"
if [[ ! "${VERSION}" =~ ^[0-9]+([.][0-9]+)+$ ]]; then
    echo "[make_release] バージョンは数字をピリオドで区切って指定すること: ${VERSION}" >&2
    exit 1
fi
if [[ ! -d "${BUILD_DIR}" ]]; then
    echo "[make_release] ビルド済みインストールが見つからない: ${BUILD_DIR}" >&2
    exit 1
fi

mkdir -p "${OUTPUT_DIR}"
OUTPUT_DIR="$(cd "${OUTPUT_DIR}" && pwd)"
ARCHIVE_NAME="${ARCHIVE_PREFIX}${VERSION}.tar.xz"
ARCHIVE_PATH="${OUTPUT_DIR}/${ARCHIVE_NAME}"
TEMP_ARCHIVE="${ARCHIVE_PATH}.tmp.$$"
INCLUDED_ENTRIES=()

cleanup() {
    if [[ -n "${COMPRESS_PID}" ]]; then
        kill "${COMPRESS_PID}" 2>/dev/null || true
    fi
    rm -f "${TEMP_ARCHIVE}"
}
trap cleanup EXIT

for entry in "${INCLUDED_DIRECTORIES[@]}"; do
    if [[ -d "${BUILD_DIR}/${entry}" ]]; then
        LINK_COUNT="$(find "${BUILD_DIR}/${entry}" -type l | wc -l)"
        if [[ "${LINK_COUNT}" -gt 0 ]]; then
            # 展開側はリンクを拒否するため、開発build内のリンクは参照先の実体として収録する。
            echo "[make_release] リンクを実体として収録: ${entry}/ (${LINK_COUNT}件)"
        fi
        INCLUDED_ENTRIES+=("${entry}")
        echo "[make_release] 収録: ${entry}/"
    else
        echo "[make_release] スキップ（存在しない）: ${entry}/"
    fi
done
for entry in "${INCLUDED_FILES[@]}"; do
    if [[ -f "${BUILD_DIR}/${entry}" ]]; then
        INCLUDED_ENTRIES+=("${entry}")
        echo "[make_release] 収録: ${entry}"
    else
        echo "[make_release] スキップ（存在しない）: ${entry}"
    fi
done

if [[ ${#INCLUDED_ENTRIES[@]} -eq 0 ]]; then
    echo "[make_release] アーカイブへ収録できる項目が無い" >&2
    exit 1
fi

EXCLUDED_LIST="$(cd "${BUILD_DIR}" && find "${INCLUDED_ENTRIES[@]}" -name "${EXCLUDED_NAME_PATTERN}" | sort)"
if [[ -n "${EXCLUDED_LIST}" ]]; then
    # 黙って落とすと後でアーカイブの中身が説明できなくなるため、除外した項目は必ず表示する。
    echo "[make_release] 除外（パスにコロンを含む）:"
    while IFS= read -r excluded_path; do
        echo "[make_release]   ${excluded_path}"
    done <<< "${EXCLUDED_LIST}"
fi

echo "[make_release] 並列圧縮を開始: ${ARCHIVE_PATH}"
START_SECONDS=${SECONDS}
(
    cd "${BUILD_DIR}"
    tar --dereference --exclude="${EXCLUDED_NAME_PATTERN}" -cf - "${INCLUDED_ENTRIES[@]}" |
        xz -T0 -c > "${TEMP_ARCHIVE}"
) &
COMPRESS_PID=$!

while kill -0 "${COMPRESS_PID}" 2>/dev/null; do
    CURRENT_SIZE=0
    if [[ -f "${TEMP_ARCHIVE}" ]]; then
        CURRENT_SIZE="$(stat -c '%s' "${TEMP_ARCHIVE}")"
    fi
    echo "[make_release] 圧縮中: 経過=$((SECONDS - START_SECONDS))秒 出力=${CURRENT_SIZE}バイト"
    sleep "${PROGRESS_INTERVAL_SECONDS}"
done
wait "${COMPRESS_PID}"
COMPRESS_PID=""

mv -f "${TEMP_ARCHIVE}" "${ARCHIVE_PATH}"
trap - EXIT
ARCHIVE_SIZE="$(stat -c '%s' "${ARCHIVE_PATH}")"
echo "[make_release] 完了: ${ARCHIVE_PATH} (${ARCHIVE_SIZE}バイト、$((SECONDS - START_SECONDS))秒)"
