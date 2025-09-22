#!/bin/sh

echo $PATH
which SetOutDir

echo "エンコード前処理 for" ${IN_PATH}
mkdir $HOME/AmatsukazeTestOut
SetOutDir "$HOME/AmatsukazeTestOut"
