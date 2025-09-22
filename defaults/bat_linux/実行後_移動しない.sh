#!/bin/bash

# IN_PATH は環境変数またはスクリプト引数から渡す
IN_PATH="${IN_PATH:-$1}"

# ディレクトリ部分を取得
IN_DIR="$(dirname "$IN_PATH")"

# succeededディレクトリが1つ上にあるか確認
if [ -d "$IN_DIR/../succeeded" ] || [ -d "$IN_DIR/../failed" ]; then
    if [ -e "${IN_PATH}.err" ]; then
        mv "${IN_PATH}.err" "$IN_DIR/.."
    fi
    if [ -e "${IN_PATH}.program.txt" ]; then
        mv "${IN_PATH}.program.txt" "$IN_DIR/.."
    fi
    if [ -e "${IN_PATH}.trim.avs" ]; then
        mv "${IN_PATH}.trim.avs" "$IN_DIR/.."
    fi
    mv "$IN_PATH" "$IN_DIR/.."
fi
