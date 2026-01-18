#!/usr/bin/env bash
set -euo pipefail

HOST="${AMT_HOST:-127.0.0.1}"
PORT="${AMT_PORT:-32769}"
BASE="http://${HOST}:${PORT}"

print_header() {
  printf "\n== %s ==\n" "$1"
}

print_result() {
  local name="$1"
  local api="$2"
  local result="$3"
  printf -- "- テスト: %s\n  API: %s\n  結果: %s\n" "$name" "$api" "$result"
}

check_json() {
  local name="$1"
  local api="$2"
  local url="$3"
  local body
  local status
  body=$(curl -sS -m 5 -w "\n%{http_code}" "$url" || true)
  status=$(printf "%s" "$body" | tail -n 1)
  body=$(printf "%s" "$body" | sed '$d')
  if [ "$status" = "200" ]; then
    print_result "$name" "$api" "OK (200)"
  else
    print_result "$name" "$api" "NG (${status})"
    return 1
  fi
}

check_csv() {
  local name="$1"
  local api="$2"
  local url="$3"
  local status
  status=$(curl -sS -m 5 -o /tmp/amt_rest_check.csv -w "%{http_code}" "$url" || true)
  if [ "$status" = "200" ]; then
    print_result "$name" "$api" "OK (200)"
  else
    print_result "$name" "$api" "NG (${status})"
    return 1
  fi
}

print_header "REST 疎通確認"
print_result "接続先" "${BASE}" "-"

print_header "基本"
failed=0
check_json "ヘルスチェック" "GET /api/health" "${BASE}/api/health" || failed=1
check_json "スナップショット" "GET /api/snapshot" "${BASE}/api/snapshot" || failed=1
check_json "システム" "GET /api/system" "${BASE}/api/system" || failed=1

print_header "キュー・ログ"
check_json "キュー" "GET /api/queue" "${BASE}/api/queue" || failed=1
check_json "エンコードログ" "GET /api/logs/encode" "${BASE}/api/logs/encode" || failed=1
check_json "チェックログ" "GET /api/logs/check" "${BASE}/api/logs/check" || failed=1
check_csv "エンコードCSV" "GET /api/logs/encode.csv" "${BASE}/api/logs/encode.csv" || failed=1
check_csv "チェックCSV" "GET /api/logs/check.csv" "${BASE}/api/logs/check.csv" || failed=1

print_header "設定・サービス"
check_json "プロファイル" "GET /api/profiles" "${BASE}/api/profiles" || failed=1
check_json "自動選択" "GET /api/autoselect" "${BASE}/api/autoselect" || failed=1
check_json "サービス" "GET /api/services" "${BASE}/api/services" || failed=1
check_json "DRCS" "GET /api/drcs" "${BASE}/api/drcs" || failed=1
check_json "設定" "GET /api/settings" "${BASE}/api/settings" || failed=1
check_json "MakeScript" "GET /api/makescript" "${BASE}/api/makescript" || failed=1
check_json "MakeScriptプレビュー" "GET /api/makescript/preview" "${BASE}/api/makescript/preview" || failed=1

print_header "コンソール"
check_json "コンソール" "GET /api/console" "${BASE}/api/console" || failed=1

print_header "完了"
if [ "$failed" -eq 0 ]; then
  print_result "サマリ" "-" "すべてのテストが完了しました"
else
  print_result "サマリ" "-" "疎通失敗がありました（詳細は上記）"
  exit 1
fi
