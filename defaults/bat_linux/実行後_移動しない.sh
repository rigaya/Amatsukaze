#!/bin/bash

# IN_PATH は環境変数またはスクリプト引数から渡す
IN_PATH="${IN_PATH:-$1}"

# ディレクトリ部分を取得
IN_DIR="$(dirname "$IN_PATH")"

# succeededディレクトリが1つ上にあるか確認
if [ -d "$IN_DIR/../succeeded" ]; then
    mv "${IN_PATH}.err" "$IN_DIR/.."
    mv "${IN_PATH}.program.txt" "$IN_DIR/.."
    mv "${IN_PATH}.trim.avs" "$IN_DIR/.."
    mv "$IN_PATH" "$IN_DIR/.."
    rmdir "$IN_DIR/../succeeded"
    rmdir "$IN_DIR/../failed"
elif [ -d "$IN_DIR/succeeded" ]; then
    rmdir "$IN_DIR/succeeded"
    rmdir "$IN_DIR/failed"
fi