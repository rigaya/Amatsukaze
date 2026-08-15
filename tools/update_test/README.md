# 本体更新のローカルテスト

Linux上で、GitHubや本番インストールを使わずにAmatsukaze本体更新を一周させるためのツールです。
`make_release.sh`はUbuntu 24.04向けLinuxアーカイブ専用で、Windows版は対象外です。

## 前提

- コマンドはリポジトリルート (`Amatsukaze/Amatsukaze`) から実行します。
- ビルド済みインストールはリポジトリの親にある`../build`を使用します。
- テスト用サーバーは42768、REST/WebUIは42769、モックリリースサーバーは8765を使います。
- 本番用の32768/32769には接続も停止操作もしません。
- `rsync`、`tar`、`xz`、Python 3が必要です。

## 1. updater入りのビルドを用意する

```bash
./scripts/build.sh ../build ../build_tmp
```

`../build/exe_files/AmatsukazeServerCLI`と`../build/AmatsukazeServer.sh`が生成されたことを確認します。

## 2. テスト用リリースアーカイブを作る

現在のバージョンより十分大きい数字を指定します。開発ビルドの比較を確実に通すには`9.9.9`のような値が便利です。

```bash
tools/update_test/make_release.sh 9.9.9 /tmp/amt_release
```

出力された`/tmp/amt_release/Amatsukaze_Ubuntu24.04_9.9.9.tar.xz`を確認します。
アーカイブ直下には`exe_files`、`JL`、`avs`、`bat`、`profile`、`scripts`、`AmatsukazeServer.sh`のうち、ビルドに存在した項目だけが入ります。親の`Amatsukaze/`ディレクトリは作りません。
展開処理はリンクを安全上拒否するため、`../build`内のシンボリックリンクは参照先の通常ファイルとして収録します。
同様に、パス要素にコロンを含むエントリも危険と判定されて展開全体が失敗します。WSL上のビルドには`gen_anime_oped_cand.py:Zone.Identifier`のようなNTFS副ストリームの痕跡が混ざることがあるため、これらは除外し、除外した項目を実行時に表示します。

必要なら構造を確認できます。

```bash
tar -tf /tmp/amt_release/Amatsukaze_Ubuntu24.04_9.9.9.tar.xz | sed -n '1,40p'
```

## 3. モックリリースサーバーを起動する

別の端末で実行し、テスト中は起動したままにします。

```bash
tools/update_test/mock_release_server.py \
  --port 8765 \
  --release-dir /tmp/amt_release \
  --version 9.9.9
```

起動時に各アセットのサイズと`sha256:`形式のダイジェストが表示されます。更新チェック時にはAPIパス、適用時には`/download/...`へのアクセスが一行ずつ表示されることを確認します。

## 4. 使い捨てテストベッドを作る

```bash
tools/update_test/setup_testbed.sh
```

既定では`/home/rigaya/dev/Amatsukaze/testbed`を作り、`../build`をコピーします。既存testbedがある場合は、削除対象を表示してから`rsync --delete`で作り直します。`log`、`artifacts`、`meson-*`はコピーしません。起動中のtestbedは先に停止してください。

別の場所を使う場合は引数で指定します。

```bash
tools/update_test/setup_testbed.sh /tmp/amatsukaze_testbed
```

誤削除防止のため、既定パス以外の空でない既存ディレクトリは、このツールが以前作成したtestbedだと確認できる場合だけ再作成できます。

## 5. テスト用サーバーを起動する

既定testbedの場合:

```bash
../testbed/update_test_server.sh start
```

このランチャーは次を固定して起動します。

- サーバーポート: 42768
- REST/WebUIポート: 42769
- `AMT_UPDATE_API_BASE_URL=http://127.0.0.1:8765/`
- `AMT_UPDATE_ALLOW_DEV_BUILD=1`

`AMT_UPDATE_ALLOW_DEV_BUILD`は開発ビルドを本体更新から除外する安全装置を外します。**本番環境では絶対に設定しないでください。**

起動状態とサーバーログは次で確認できます。

```bash
../testbed/update_test_server.sh status
../testbed/update_test_server.sh log
```

## 6. 更新チェックと適用を行う

ブラウザで次を開きます。

```text
http://127.0.0.1:42769/settings?update=1
```

更新チェックを実行し、Amatsukaze本体が`9.9.9`への更新として表示されることを確認します。更新ログは次の場所です。

```text
<testbed>/log/update/*.log
```

確認する主なステージ:

- `S00_ENV`: `allow_dev_build=yes`
- `S03_CONNECT`: `host=127.0.0.1`
- `S04_LATEST`: `tag=9.9.9`
- `S05_SELECT_ASSET`: Ubuntu 24.04用アーカイブとダイジェスト
- `S06_DOWNLOAD`: モックサーバーから全体を取得
- `S07_VERIFY`: サイズとSHA-256が一致
- `S08_EXTRACT`: 展開成功
- `S09_STAGE`: トップレベルと製品ファイルの検証成功
- `S20_WAIT_EXIT`: 元のサーバープロセスの終了待ち
- `S21_BACKUP`: 置換前バックアップ
- `S22_PLACE`: 新しいファイルの配置と作業領域の削除
- `S23_ROLLBACK`: 成功時は`SKIP`、失敗時は復元結果
- `S24_RESTART`: サーバー再起動
- `S29_RESULT`: updaterと再起動後の結果取り込みで`status=success`

「更新する」を実行するとサーバーはいったん終了し、自己更新スクリプトがファイルを置換して同じ42768/42769で再起動します。

## 7. 成功を確認する

次をすべて満たせば一周成功です。

1. `http://127.0.0.1:42769/`が再び応答する。
2. `../testbed/update_test_server.sh status`が`ready`を返す。
3. 最新の`<testbed>/log/update/*.log`に`S29_RESULT OK status=success version=9.9.9`がある。
4. `.amatsukaze_update/staging`が残らず、WebUIに保留中の本体更新が表示されない。
5. 再起動後も更新ダイアログを開ける。アーカイブ自体がupdaterブランチのビルドなので、更新機能が失われていないことを確認できる。

アーカイブの中身は元の開発ビルドと同じため、実行ファイルが報告するバージョン文字列は`9.9.9`には変わりません。成功判定には更新ログの期待バージョンと再起動後の結果を使います。

## 停止とやり直し

停止:

```bash
../testbed/update_test_server.sh stop
```

失敗したtestbedは修復せず、停止後に作り直します。

```bash
../testbed/update_test_server.sh stop || true
tools/update_test/setup_testbed.sh
../testbed/update_test_server.sh start
```

モックサーバーは`Ctrl+C`で停止します。リリース内容を変更した場合は、モックサーバーも再起動してサイズとダイジェストを再計算してください。
