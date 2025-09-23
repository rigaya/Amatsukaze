#!/bin/bash

# OUT_PATH環境変数からディレクトリパスを取得し、「メイン動画以外」サブディレクトリを作成
DST="${OUT_PATH}/メイン動画以外"
mkdir -p "$DST"

# GetOutFiles cwdtlコマンドでファイルリストを取得
FILES=$(GetOutFiles cwdtl)

# セミコロン区切りのファイルリストを処理
IFS=';' read -ra FILE_ARRAY <<< "$FILES"
for file in "${FILE_ARRAY[@]}"; do
    if [ -n "$file" ]; then
        mv "$file" "$DST/"
    fi
done
