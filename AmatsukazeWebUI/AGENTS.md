## WebUI / REST 実装メモ

- 保守性優先で、依存関係を最小限にする方針（UIライブラリ不使用、JS interop最小）。
- WebUIは ```AmatsukazeWebUI/```（Blazor WASM）で、RESTは ```AmatsukazeServer/Server/Rest/``` に集約。
- DTO/共通APIは ```AmatsukazeShared/``` に置く。WebUI/Serverの両方で参照する。
- WebUIのUI調整は ```AmatsukazeWebUI/wwwroot/css/app.css``` が集約ポイント。
  - ライト/ダークは CSS変数で切替（```prefers-color-scheme```）。
  - Bootstrapの背景/文字色は ```--bs-*``` を上書きして統一（例: ```--bs-body-bg```, ```--bs-emphasis-color```）。

## サーバー側の不整合防止（null/重複ID対策）

- キュー操作・保存・ID発行は排他制御で保護（`queueSync` でロック）。
- 保存時に null/SrcPath 欠損アイテムを除外し、重複IDは再採番してから保存する。
- 起動時の読み込みでも null/SrcPath 欠損アイテムは破棄し、IDを振り直す。
- REST側は null/SrcPath 欠損をスキップし、APIが落ちないようにする。

## WebUIの(半)リアルタイム更新

- WebUIのリアルタイム更新は以下の方式が基本:
  - Queue: 差分ポーリング（```/api/queue/changes```） + 必要時フルスナップショット。
  - System: ```/api/system``` を独立ポーリングし、Queueページで稼働/停止などを同期更新。
- リアルタイム更新の注意点:
  - キュー更新は「バージョン」＋「変更一覧」前提。バージョン不整合時はフル同期が必要。
  - 差分適用では「Item.Idをキーに更新」し、ステータス/カウンタの更新漏れに注意。
  - 一時的に不整合が出る場合があるため、サーバーの変更バッファ（リングバッファ長）とクライアントのsince/toの扱いを確認すること。
  - 変更が反映されない場合は、まず「差分レスポンスの型不一致」「versionの更新漏れ」「クライアント側の差分適用漏れ」を疑う。

## 各ページについて

- タスクキュー関連(ホーム):
  - キュー表示/操作は `AmatsukazeWebUI/Pages/Queue.razor` に集約。
  - リアルタイム更新は `GetQueueChangesAsync` で差分取得、必要時は `GetQueueAsync` のスナップショットで補正。
  - WebUI側のキュー操作は基本的にIDベースで行う（選択・右クリック操作・ドラッグ移動）。
    - リスト入れ替え（D&D）対応
      - 単一移動は `ChangeItemType.Move` を使用。
      - 複数選択の移動は `MoveQueueManyAsync` を使用し、移動対象は選択ID集合＋`dropIndex` を送る。
      - `dropIndex` は「テーブル上の行インデックス」（末尾は `Count`）をそのまま送る。
      - サーバー側で移動を一括処理し、最終順序に沿って `QueueChangeType.Move` を複数送信する。
  - 追加タスクは ```/api/queue/add``` を使用（DirPath/Targets/OutDir/Mode/Batch等）。
    - 入力ファイル指定には基本設定でも使用するパス補間
  - キュー順序入替は Drag&Drop を使用。サーバー側の順序更新APIが必要。

- 基本設定
  - WPF SettingPanel と同等の項目構成。
  - パス入力欄にはサーバー側のファイル/ディレクトリパス補間
    - ```/api/path/suggest``` を使用。AllowFiles/AllowDirs, 拡張子フィルタ指定。
    - UIは ```PathSuggestInput``` コンポーネントを使用。

- プロファイル
  - WPFのプロファイル編集画面と同等。上部固定のプロファイル操作バー。

- サービス関連:
  - ロゴ/サービス更新は REST経由で実施。ロゴURLは ```/api/assets/...``` をフルURLで組み立てること。
  - ロゴ追加後は「ロゴ再スキャン要求API」を叩いて反映する。
  - ロゴアップロードはサーバー側で形式判定（Amatsukaze拡張/AviUtl）し、AviUtl時のみ imgw/imgh 入力。

- 自動選択
  - 条件リスト編集（D&D並べ替え）、ジャンル/チャンネル/サイズをサーバー取得。

- DRCS外字
  - 画像＋MapStr編集、追加/削除、出現位置確認（ツールチップ）。

- スクリプト生成
  - サーバー側生成統一。リモート/ローカル切替、WOL/NAS入力、Windows bat / Linux sh 出力。

- ロゴ解析:
  - デコード/解析/生成はサーバー側、WebUIは表示と操作のみ。
  - シークプレビューはバイトシーク/フレーム取得をネイティブ側で保持し連続シーク対応。

- エンコードログ / チェックログ
  - 詳細は右側レイヤーで表示（x/外側クリックで閉じる）。

- ログ/コンソール: 表示のみのログビュー。
  - TaskIdベースの単一タスク表示に変更（ConsoleIdは外部に出さない）。
  - `/api/console/{taskId}` スナップショット + `/api/console/{taskId}/changes` 差分ポーリングで更新。
  - CR進捗はReplace扱いで上書き表示。最大1000行を保持。
  - 終了済みタスクはログファイルを読み込み表示し、リトライ時はライブ表示に切替。

- 情報
  - サーバーのホスト名/バージョン/最新版/OS
  - ディスク情報
    - ディスク情報はたまに猛烈に取得に時間がかかるので別APIにして他への影響緩和

## 配布/起動（WebUI 統合）:
- WebUI は REST サーバから静的配信（Blazor WASM）。WPF/CLI のTCPポートと競合するため **REST/WebUI は port+1** を使用。
- `RestApiHost` で `UseBlazorFrameworkFiles()` + `UseStaticFiles()` + `MapFallbackToFile("index.html")` を有効化。
- `RestApiHost` の `WebRootPath` は `AppContext.BaseDirectory/wwwroot` を参照（配布先 `exe_files/wwwroot`）。
- `AmatsukazeServer.csproj` に AfterPublish を追加し、WebUI publish → `wwwroot` を Server publish にコピー。
  - Linux: `scripts/build.sh` で `AmatsukazeServer` を publish（SingleFile=false）し `exe_files/wwwroot` へ配置。
  - Windows: `Publish.proj` に `AmatsukazeServer` publish を追加し、`publish.bat` で `wwwroot` をマージ。
- WebUI のURLは `http://<host>:<port+1>/`。API は `http://<host>:<port+1>/api` ではなく **port+1 と同一ホストの別ポート**で動くため、CORS は LAN IP 許可で運用。
