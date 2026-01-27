## 構成

Amatsukazeはクライアント・サーバーアーキテクチャに基づくアプリケーションです。

Amatsukazeは以下のようにして構成されます。

- ```AmatsuakzeServer/```

  Amatsukazeのサーバー。キューを持ち、AmatsuakzeCLIを駆動して、キューに登録されたタスクの処理を行う。
  
  - C# (.NET Core)
  - Windows/Linux両対応

- ```AmatsuakzeGUI/```

  Amatsuakzeのフロントエンドの実装。
  
  - C# (WPF)
  - Windows専用
  
  - 実行方法

    - ```AmatsukazeGUI.exe -l server```

      AmatsuakzeServerのGUI版が起動。
    
    - ```AmatsukazeGUI.exe -l client```

      AmatsukazeGUIがclientとしてのみ起動。別途起動したserverに接続して使用。このサーバーは ```AmatsukazeGUI.exe -l server``` あるいは ```AmatsuakzeServerCLI``` で起動したもの。
    
    - ```AmatsukazeGUI.exe -l standalone```

    スタンドアロン版。実態はclientとserverが両方起動。

- ```AmatsuakzeServerCLI/```

  サーバーのコンソールアプリ版。
  
  - C# (.NET Core)
  - Windows/Linux両対応

- ```AmatsuakzeServerWin/```

  AmatsukazeServerのWPFが必要な処理の実装。
  
  - C# (WPF)
  - Windows専用

- ```AmatsuakzeAddTask/```

  タスクをサーバーに追加するためのコンソールアプリ。
  
  - C# (.NET Core)
  - Windows/Linux両対応

- ```AmatsukazeShared/```

  REST API向けDTOとクライアント共通APIラッパーを提供する共有ライブラリ。
  
  - C# (.NET)
  - Windows/Linux両対応

- ```AmatsukazeWebUI/```

  Blazor WebAssemblyによるWebフロントエンド。AmatsukazeServerのREST APIを利用する。
  
  - C# (Blazor WASM)
  - Windows/Linux両対応（ブラウザ動作）

- ```Amatsukaze/```
   タスク処理の実体部分。Amatsukaze[.dll,.so]/Amatsukaze2[.dll,.so]としてビルドされ、AmatsuakzeCLIにロードされる。
  
  - C++17
  - Windows/Linux両対応
  - TCHARでUnicode対応

- ```AmatsuakzeCLI/```

  タスク処理を行うコンソールアプリ。
  Amatsukaze[.dll,.so]/Amatsukaze2[.dll,.so]をロードして実行。

  - C++17
  - Windows/Linux両対応

- ```common/```
 C++のutility関数等。```Amatsukaze/```で使用される。OS互換ラッパー等を含む。C++17までの標準機能を優先しつつ、場合によってはWindows側によせるためのラッパー実装。
  
  - C++17
  - Windows/Linux両対応
  
- ```ScriptCommand/```

  ```AmatsuakzeServer```, ```AmatsuakzeCLI``` の行うユーザーバッチ処理実行で、使用可能なコンソールアプリ。```AmatsuakzeServer```から情報取得等が可能。
  
  - C++17
  - Windows/Linux両対応
  
- ```ScriptCommandWrapper/```

  ```ScriptCommand```を呼び出す補助コンソールアプリ。
  
  - C++17
  - Windows用
  
- ```doc/```

  ドキュメント類。
  
- ```data/```

  ドキュメントその他で必要な画像等の静的リソース。
  
- ```scripts/```

  ビルドや配布アーカイブ生成に必要なスクリプト類。
  
- ```docker/```

  - ```Dockerfile```
  - ```compose.sample.yml```
  - ```setup.sh```
  - ```readme.md```
    AmatsukazeServer実行用のdockerコンテナの作成用。```readme.md```がその使用方法
	
  - ```docker_ubuntu2204```
  - ```docker_ubuntu2404```
  
    Linux用配布パッケージのビルド用のdockerfile。CIで使用。
  
- ```defaults/```

  配布用デフォルト設定。

- ```.github/workflows```

  - ```build_base_image.yml```
  
    ```build_packages.yml```で使用するLinux用配布パッケージをビルド・作成するためのdocker imageを作成する。
  
  - ```build_packages.yml```
  
    ```build_base_image.yml```で作成したimage上で、Linux用配布パッケージをビルド・作成する。
  
  - ```build_amatsukazeaddtask.yml```
  
    AmatsuakzeAddTaskのみのパッケージの作成を行う。

- ```TVCaption2Mod/```
- ```libfaad/```

  ```Amatsukaze/```の依存モジュール。原則ビルドするのみで開発/変更は行わない。
  
- ```AmatsukazeUnitTest/```
- ```BatchHashChecker/```
- ```FileCutter/```
- ```googletest/```
- ```NicoJK18Client/```

  改造版では開発/変更は行わない。
  
## ビルド

- Windows

  ```publish.bat``` でビルド

- Linux

  ```doc/BuildLinux.md``` を参照

## 実装方針

- 日本語のコメントを多めに記述する
- 既存のコメントはそのまま保持する。ただし、変更によりコメント内容がコードの記述と合わなくなった場合は削除せず、適切に変更する
- ログメッセージを適切に記述すること
- 既存のラッパー関数があれば、それを積極的に使用し、コードの統一性を保つこと
- Windows/Linuxの差異に注意すること
  - 可能な限りWindows/Linux共通実装を心がける。
  - (C++)
    - Windows/Linuxの固有関数を呼び出す必要がある場合、```common/```以下のラッパー関数、次にC++17標準の使用を推奨。
      - 困難な場合は```common/```以下に新たにラッパーを作り、ラッパー経由で呼び出すようにする。
    
- マジックナンバーは使用せず、意味が分かるように定数変数を置いて使用すること
- コーディングスタイルは、変更箇所周辺のコードスタイルに合わせる
- コードが縦に長くなりすぎないよう配慮すること (過剰に改行しない)
- (C++) constを積極的に使用する

## WebUI / REST 実装メモ

- 保守性優先で、依存関係を最小限にする方針（UIライブラリ不使用、JS interop最小）。
- WebUIは ```AmatsukazeWebUI/```（Blazor WASM）で、RESTは ```AmatsukazeServer/Server/Rest/``` に集約。
- DTO/共通APIは ```AmatsukazeShared/``` に置く。WebUI/Serverの両方で参照する。
- WebUIのUI調整は ```AmatsukazeWebUI/wwwroot/css/app.css``` が集約ポイント。
  - ライト/ダークは CSS変数で切替（```prefers-color-scheme```）。
  - Bootstrapの背景/文字色は ```--bs-*``` を上書きして統一（例: ```--bs-body-bg```, ```--bs-emphasis-color```）。
- Settingsページのレイアウトは以下で調整:
  - ページ本体を ```<div class="settings-page">``` で包み、```app.css``` の ```settings-page``` スタイルで幅/余白を制御。
  - チェックボックスは「右ラベル＋1列フル幅」に統一（```settings-check-row``` / ```settings-check```）。
  - 大項目間の区切りは ```<hr class="settings-sep">```。
  - 実行時間帯は可変グリッド（```run-hours-grid```）+ セルクリックでON/OFF。
    - クリックはループ変数のキャプチャに注意（```var hour = i;``` を使う）。
- WebUIのリアルタイム更新は以下の方式が基本:
  - Queue: 差分ポーリング（```/api/queue/changes```） + 必要時フルスナップショット。
  - System: ```/api/system``` を独立ポーリングし、Queueページで稼働/停止などを同期更新。
- リアルタイム更新の注意点:
  - キュー更新は「バージョン」＋「変更一覧」前提。バージョン不整合時はフル同期が必要。
  - 差分適用では「Item.Idをキーに更新」し、ステータス/カウンタの更新漏れに注意。
  - 一時的に不整合が出る場合があるため、サーバーの変更バッファ（リングバッファ長）とクライアントのsince/toの扱いを確認すること。
  - 変更が反映されない場合は、まず「差分レスポンスの型不一致」「versionの更新漏れ」「クライアント側の差分適用漏れ」を疑う。
- Queue関連:
  - 追加タスクは ```/api/queue/add``` を使用（DirPath/Targets/OutDir/Mode/Batch等）。
  - キュー順序入替は Drag&Drop を使用。サーバー側の順序更新APIが必要。
- Services関連:
  - ロゴ/サービス更新は REST経由で実施。ロゴURLは ```/api/assets/...``` をフルURLで組み立てること。
  - ロゴ追加後は「ロゴ再スキャン要求API」を叩いて反映する。
- Path補間:
  - ```/api/path/suggest``` を使用。AllowFiles/AllowDirs, 拡張子フィルタ指定。
  - UIは ```PathSuggestInput``` コンポーネントを使用。
- ロゴ解析:
  - デコード/解析/生成はサーバー側、WebUIは表示と操作のみ。
  - シークプレビューはバイトシーク/フレーム取得をネイティブ側で保持し連続シーク対応。
- HTML5 DnD:
  - 原則JS interopは使わない方針だが、条件リストのドラッグ＆ドロップでのみHTML5 DnDを成立させるために限定的に使用。
- 各ページの実装ポイント:
  - Home: 旧Statusを統合。上部トグル（キュー稼働/エンコーダOK）＋処理後動作を表示・変更、リアルタイム更新は独立ポーリング。
  - Queue: 右クリックで操作メニュー（Retry/Cancel/削除/強制実行/Logo等）。単一D&Dで順序変更対応（複数移動は未対応）。
    - タスク表示は 3列構成（タスク/放送日/チャンネル、入力ファイル、状態/優先度/操作）。ツールチップにフル情報。
  - Console: 表示のみのログビュー。
  - Encode Logs / Check Logs: 分離済み。詳細は右側レイヤーで表示（x/外側クリックで閉じる）。ページング対応は別途検討中。
  - Services（チャンネル設定）: サービス/ロゴの管理（追加・削除・JoinLogoScp、ロゴ期間、有効/無効切替、ロゴ再スキャン）。
    - ロゴアップロードはサーバー側で形式判定（Amatsukaze拡張/AviUtl）し、AviUtl時のみ imgw/imgh 入力。
  - Settings（基本設定）: WPF SettingPanel と同等の項目構成。依存関係の表示/非表示、OS別非表示を反映。
    - パス補間（PathSuggestInput）を全パス入力に適用。
  - Profiles（プロファイル）: 上部固定のプロファイル操作バー。設定は段階実装（Stage2-1～）。
  - AutoSelect（自動選択）: 条件リスト編集（D&D並べ替え）、ジャンル/チャンネル/サイズをサーバー取得。
  - DRCS: 画像＋MapStr編集、追加/削除、出現位置確認（ツールチップ）。
  - MakeScript（スクリプト生成）: サーバー側生成統一。リモート/ローカル切替、WOL/NAS入力、Windows bat / Linux sh 出力。
  - Logo Analyze: 解析はサーバー側、WebUIはシーク/範囲指定/進捗/結果採用。
- 配布/起動（WebUI 統合）:
  - WebUI は REST サーバから静的配信（Blazor WASM）。WPF/CLI のTCPポートと競合するため **REST/WebUI は port+1** を使用。
  - `RestApiHost` で `UseBlazorFrameworkFiles()` + `UseStaticFiles()` + `MapFallbackToFile("index.html")` を有効化。
  - `RestApiHost` の `WebRootPath` は `AppContext.BaseDirectory/wwwroot` を参照（配布先 `exe_files/wwwroot`）。
  - `AmatsukazeServer.csproj` に AfterPublish を追加し、WebUI publish → `wwwroot` を Server publish にコピー。
  - Linux: `scripts/build.sh` で `AmatsukazeServer` を publish（SingleFile=false）し `exe_files/wwwroot` へ配置。
  - Windows: `Publish.proj` に `AmatsukazeServer` publish を追加し、`publish.bat` で `wwwroot` をマージ。
  - WebUI のURLは `http://<host>:<port+1>/`。API は `http://<host>:<port+1>/api` ではなく **port+1 と同一ホストの別ポート**で動くため、CORS は LAN IP 許可で運用。
