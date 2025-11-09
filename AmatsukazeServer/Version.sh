#!/bin/bash
VER=$(git describe --tags)
# バージョン番号を正しい形式に変換（例: 0.9.8.7-28-g7f676f2 -> 0.9.8.7）
SHORTVER=$(echo $VER | sed -E 's/-[0-9]+-g[0-9a-f]+$//')

# テンプレートからAssemblyInfo.csを生成（差分がない場合は更新しない）
TEMPLATE="Properties/AssemblyInfo.tt"
OUTPUT="Properties/AssemblyInfo.cs"
TMP="$(mktemp 2>/dev/null || printf "%s" "${OUTPUT}.tmp.$$.$RANDOM")"

sed -e "s/@VERSION@/$VER/g" -e "s/@SHORTVERSION@/$SHORTVER/g" "$TEMPLATE" > "$TMP"

if [ -f "$OUTPUT" ]; then
  if cmp -s "$TMP" "$OUTPUT"; then
    rm -f "$TMP"
    exit 0
  fi
fi

mv -f "$TMP" "$OUTPUT"