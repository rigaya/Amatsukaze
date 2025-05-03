#!/bin/bash
VER=$(git describe --tags)
# バージョン番号を正しい形式に変換（例: 0.9.8.7-28-g7f676f2 -> 0.9.8.7）
SHORTVER=$(echo $VER | sed -E 's/-[0-9]+-g[0-9a-f]+$//')

# テンプレートからAssemblyInfo.csを生成
sed -e "s/@VERSION@/$VER/g" -e "s/@SHORTVERSION@/$SHORTVER/g" Properties/AssemblyInfo.tt > Properties/AssemblyInfo.cs 