#!/bin/bash

# IN_PATH �͊��ϐ��܂��̓X�N���v�g��������n��
IN_PATH="${IN_PATH:-$1}"

# �f�B���N�g���������擾
IN_DIR="$(dirname "$IN_PATH")"

# succeeded�f�B���N�g����1��ɂ��邩�m�F
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