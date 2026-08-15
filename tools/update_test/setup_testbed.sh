#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPOSITORY_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
WORKSPACE_ROOT="$(cd "${REPOSITORY_ROOT}/.." && pwd)"
SOURCE_BUILD_DIR="${WORKSPACE_ROOT}/build"
DEFAULT_TESTBED_DIR="${WORKSPACE_ROOT}/testbed"
TESTBED_DIR="${1:-${DEFAULT_TESTBED_DIR}}"
LAUNCHER_NAME="update_test_server.sh"
TESTBED_MARKER=".amatsukaze-update-testbed"
TEST_SERVER_PORT=42768
EXCLUDED_PATHS=(log artifacts 'meson-*')
REQUIRED_COMMANDS=(rsync curl pgrep setsid realpath find awk sed ps tail tr)

if [[ $# -gt 1 ]]; then
    echo "使い方: setup_testbed.sh [テストベッドのパス]" >&2
    exit 1
fi
for command_name in "${REQUIRED_COMMANDS[@]}"; do
    if ! command -v "${command_name}" >/dev/null 2>&1; then
        echo "[setup_testbed] 必要なコマンドが見つからない: ${command_name}" >&2
        exit 1
    fi
done
if [[ ! -x "${SOURCE_BUILD_DIR}/exe_files/AmatsukazeServerCLI" ]]; then
    echo "[setup_testbed] ビルド済みサーバーが見つからない: ${SOURCE_BUILD_DIR}" >&2
    exit 1
fi

TESTBED_DIR="$(realpath -m "${TESTBED_DIR}")"
SOURCE_BUILD_DIR="$(realpath "${SOURCE_BUILD_DIR}")"
case "${TESTBED_DIR}" in
    /|"${WORKSPACE_ROOT}"|"${REPOSITORY_ROOT}"|"${SOURCE_BUILD_DIR}")
        echo "[setup_testbed] 安全でないテストベッドパスを拒否した: ${TESTBED_DIR}" >&2
        exit 1
        ;;
esac
if [[ "${SOURCE_BUILD_DIR}" == "${TESTBED_DIR}"/* ||
      "${TESTBED_DIR}" == "${SOURCE_BUILD_DIR}"/* ]]; then
    echo "[setup_testbed] コピー元と包含関係にあるパスは指定できない: ${TESTBED_DIR}" >&2
    exit 1
fi

RUNNING_TESTBED_PIDS="$(pgrep -f -- \
    "${TESTBED_DIR}/exe_files/AmatsukazeServerCLI -p ${TEST_SERVER_PORT}" || true)"
if [[ -n "${RUNNING_TESTBED_PIDS}" ]]; then
    echo "[setup_testbed] testbedのサーバーを停止してから再作成すること: pid=${RUNNING_TESTBED_PIDS//$'\n'/,}" >&2
    exit 1
fi

RSYNC_EXCLUDES=()
for path in "${EXCLUDED_PATHS[@]}"; do
    RSYNC_EXCLUDES+=("--exclude=/${path}")
done

if [[ -e "${TESTBED_DIR}" ]]; then
    if [[ "${TESTBED_DIR}" != "${DEFAULT_TESTBED_DIR}" &&
          ! -f "${TESTBED_DIR}/${TESTBED_MARKER}" &&
          -n "$(find "${TESTBED_DIR}" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
        echo "[setup_testbed] このツールが作成したと確認できない既存パスは削除できない: ${TESTBED_DIR}" >&2
        echo "[setup_testbed] 空のディレクトリか、新しいパスを指定すること" >&2
        exit 1
    fi
    echo "[setup_testbed] 再作成対象: ${TESTBED_DIR}"
    echo "[setup_testbed] rsync --deleteにより、コピー元に無い既存内容を削除する"
    DELETE_PREVIEW="$(rsync -ain --delete --delete-excluded "${RSYNC_EXCLUDES[@]}" \
        "${SOURCE_BUILD_DIR}/" "${TESTBED_DIR}/" | awk '$1 == "*deleting" { print $2 }')"
    if [[ -n "${DELETE_PREVIEW}" ]]; then
        echo "[setup_testbed] 削除予定:"
        while IFS= read -r deleted_path; do
            echo "[setup_testbed]   ${TESTBED_DIR}/${deleted_path}"
        done <<< "${DELETE_PREVIEW}"
    else
        echo "[setup_testbed] 削除予定: なし"
    fi
else
    echo "[setup_testbed] 新規作成: ${TESTBED_DIR}"
fi
mkdir -p "${TESTBED_DIR}"

rsync -a --delete --delete-excluded "${RSYNC_EXCLUDES[@]}" \
    "${SOURCE_BUILD_DIR}/" "${TESTBED_DIR}/"
printf '%s\n' "Amatsukaze本体更新テスト用" > "${TESTBED_DIR}/${TESTBED_MARKER}"

cat > "${TESTBED_DIR}/${LAUNCHER_NAME}" <<'LAUNCHER'
#!/usr/bin/env bash

set -euo pipefail

TESTBED_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVER_CLI="${TESTBED_DIR}/exe_files/AmatsukazeServerCLI"
SERVER_PORT=42768
REST_PORT=42769
MOCK_RELEASE_PORT=8765
UPDATE_API_BASE_URL="http://127.0.0.1:${MOCK_RELEASE_PORT}/"
READY_URL="http://127.0.0.1:${REST_PORT}/"
PID_FILE="${TESTBED_DIR}/.update-test-server.pid"
SERVER_LOG="${TESTBED_DIR}/update-test-server.log"
READY_TIMEOUT_SECONDS=30
STOP_TIMEOUT_SECONDS=15
POLL_INTERVAL_SECONDS=1
HTTP_PROBE_TIMEOUT_SECONDS=2
ERROR_LOG_LINES=80
FOLLOW_LOG_LINES=100

find_server_pids() {
    pgrep -f -- "${SERVER_CLI} -p ${SERVER_PORT}" || true
}

is_ready() {
    curl -fsS --max-time "${HTTP_PROBE_TIMEOUT_SECONDS}" "${READY_URL}" >/dev/null 2>&1
}

wait_ready() {
    local deadline=$((SECONDS + READY_TIMEOUT_SECONDS))
    while ((SECONDS < deadline)); do
        if is_ready; then
            return 0
        fi
        sleep "${POLL_INTERVAL_SECONDS}"
    done
    return 1
}

start_server() {
    if [[ ! -x "${SERVER_CLI}" ]]; then
        echo "[update_test_server] サーバー実行ファイルが見つからない: ${SERVER_CLI}" >&2
        return 1
    fi
    local pids
    pids="$(find_server_pids | tr '\n' ' ' | sed 's/[[:space:]]*$//')"
    if is_ready; then
        if [[ -z "${pids}" ]]; then
            echo "[update_test_server] ${READY_URL}は応答するが、testbedのプロセスではない" >&2
            return 1
        fi
        echo "[update_test_server] 起動済み: pid=${pids} url=${READY_URL}"
        return 0
    fi
    if [[ -n "${pids}" ]]; then
        echo "[update_test_server] 応答しない既存プロセスを停止する: pid=${pids}" >&2
        stop_server
    fi

    (
        cd "${TESTBED_DIR}"
        export AMT_UPDATE_API_BASE_URL="${UPDATE_API_BASE_URL}"
        export AMT_UPDATE_ALLOW_DEV_BUILD=1
        export AMT_REST_PORT="${REST_PORT}"
        setsid "${SERVER_CLI}" -p "${SERVER_PORT}" </dev/null >>"${SERVER_LOG}" 2>&1 &
        echo $! > "${PID_FILE}"
    )
    if wait_ready; then
        echo "[update_test_server] 起動完了: url=${READY_URL} log=${SERVER_LOG}"
        return 0
    fi
    echo "[update_test_server] 起動確認に失敗: ${READY_URL}" >&2
    tail -n "${ERROR_LOG_LINES}" "${SERVER_LOG}" >&2 || true
    return 1
}

stop_server() {
    local pids=()
    if [[ -f "${PID_FILE}" ]]; then
        local recorded_pid
        recorded_pid="$(cat "${PID_FILE}" 2>/dev/null || true)"
        local recorded_command=""
        if [[ -n "${recorded_pid}" ]]; then
            recorded_command="$(ps -p "${recorded_pid}" -o args= 2>/dev/null || true)"
        fi
        if [[ "${recorded_command}" == *"${SERVER_CLI} -p ${SERVER_PORT}"* ]]; then
            pids+=("${recorded_pid}")
        fi
    fi
    while IFS= read -r pid; do
        [[ -n "${pid}" ]] && pids+=("${pid}")
    done < <(find_server_pids)
    if [[ ${#pids[@]} -eq 0 ]]; then
        rm -f "${PID_FILE}"
        echo "[update_test_server] 停止対象なし"
        return 0
    fi
    mapfile -t pids < <(printf '%s\n' "${pids[@]}" | awk '!seen[$0]++')
    kill "${pids[@]}" 2>/dev/null || true
    local deadline=$((SECONDS + STOP_TIMEOUT_SECONDS))
    while ((SECONDS < deadline)); do
        local alive=0
        for pid in "${pids[@]}"; do
            if kill -0 "${pid}" 2>/dev/null; then
                alive=1
                break
            fi
        done
        if ((alive == 0)); then
            rm -f "${PID_FILE}"
            echo "[update_test_server] 停止完了: pid=${pids[*]}"
            return 0
        fi
        sleep "${POLL_INTERVAL_SECONDS}"
    done
    echo "[update_test_server] 停止タイムアウト: pid=${pids[*]}" >&2
    return 1
}

status_server() {
    local pids
    pids="$(find_server_pids | tr '\n' ' ' | sed 's/[[:space:]]*$//')"
    if is_ready && [[ -n "${pids}" ]]; then
        echo "[update_test_server] ready: pid=${pids} url=${READY_URL}"
        return 0
    fi
    echo "[update_test_server] stoppedまたは未応答: pid=${pids:-none} url=${READY_URL}" >&2
    return 1
}

case "${1:-start}" in
    start) start_server ;;
    stop) stop_server ;;
    restart) stop_server; start_server ;;
    status) status_server ;;
    wait) wait_ready && status_server ;;
    log) tail -n "${FOLLOW_LOG_LINES}" -f "${SERVER_LOG}" ;;
    *)
        echo "使い方: ${BASH_SOURCE[0]} {start|stop|restart|status|wait|log}" >&2
        exit 1
        ;;
esac
LAUNCHER

chmod +x "${TESTBED_DIR}/${LAUNCHER_NAME}"
echo "[setup_testbed] 完了: ${TESTBED_DIR}"
echo "[setup_testbed] 起動: ${TESTBED_DIR}/${LAUNCHER_NAME} start"
echo "[setup_testbed] WebUI: http://127.0.0.1:42769/"
