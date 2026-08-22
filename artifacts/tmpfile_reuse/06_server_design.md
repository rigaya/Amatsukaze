# Step 3 サーバー側連携 設計方針

作成: 2026-07-10 / 根拠: 05_server_survey.md / 前提: C++側 d1314ab

## 目的

カット調整のタスク再投入時に、元CM解析タスクの一時フォルダを `--resume-dir` として
新Batchタスクへ引き継ぎ、共通処理スキップを実運用に載せる。

## 設計判断

### A. 専用再投入APIを新設する(AddQueue拡張ではなく)

`POST /api/trim/requeue`(仮)を新設し、WebUIの `RequeueWithProfile` をこれに切り替える。

- 入力: 元CMアイテムのqueueItemId、プロファイル、優先度、タグ、元CMタスク削除フラグ
- サーバー側処理(TrimAdjustService内で原子的に):
  1. 元アイテムが完了済みCMCheck/Batchであることを検証
  2. ログから一時フォルダを抽出(既存 `ExtractTempDirFromLog` を流用)し、
     `resume.dat` の存在を確認(なければResumeDirなしの通常再投入に落とす)
  3. 新Batchの `QueueItem` を生成し、`ResumeDir` を永続化して登録
  4. 削除フラグが真なら元CMアイテムをキューから削除(**フォルダは削除しない**)
- 理由: クライアントから任意パスを受けるAddQueue拡張はパス注入と削除競合の余地がある。
  一時フォルダの知識はサーバー(TrimAdjustService)に既にあるので、クライアントに
  往復させない方が安全で単純。

### B. QueueItem.ResumeDir の永続化と引数化

- `QueueItem` に `ResumeDir`(string、DataContract)を追加。待機・サーバー再起動をまたいで保持。
- `TranscodeWorker` → `MakeAmatsukazeArgs` で、`ResumeDir` が非空かつ通常Batch系のときだけ
  `--resume-dir "<path>"` を付与。mode=cm/drcs/gには付けない。
- 実行直前に `Directory.Exists(ResumeDir)` を確認し、消えていたら引数を付けずに警告ログ
  (CLI側にもフォールバックがあるので二重の安全)。

### C. 一時フォルダの所有権と寿命

「ResumeDirは再投入されたBatchタスクが所有する」を原則にする。

| 契機 | 挙動 |
|---|---|
| 再投入時(`TrimAdjustDeleteCmTask`=true) | 元CMアイテムはキューから削除してよいが、一時フォルダの即時 `Directory.Delete` は行わない(現行 TrimAdjust.razor:1693-1715 の削除をサーバー側APIに移した上で抑止) |
| 再利用Batchの正常終了 | プロファイルの `NoRemoveTmp` が偽なら、CLIの既存デストラクタがResumeDir(=一時フォルダ)を削除。サーバー側の追加処理不要 |
| 再利用Batchの失敗・取消 | フォルダは残す(再試行・再カット調整が可能)。キュー削除時の既存 `DeleteTaskWorkDirOnQueueRemove` の安全検査(`amt<数字>`・非リンク)がそのまま働く |
| `ClearWorkDirOnStart` | キュー内アイテムの `ResumeDir` に含まれるフォルダを削除対象から除外する |

### D. 変更しないこと

- CLIの通知形式・出力JSON(一時フォルダはログ抽出で足りる)
- CMCheck時の `--no-remove-tmp` 強制付与はしない(現状どおりプロファイルの `NoRemoveTmp` 前提。
  カット調整自体が既にこの前提で動いているため、前提は増えない)
- 既存の `/api/queue/add` のDTO(変更なし)

## 変更ファイル見立て

| 箇所 | 変更 |
|---|---|
| AmatsukazeServer/Server/EncodeServerData.cs | `QueueItem.ResumeDir` 追加(DataContract) |
| AmatsukazeServer/Server/Rest/TrimAdjustService.cs | 再投入処理の実体(検証・QueueItem生成・元タスク削除)。既存の即時フォルダ削除経路の抑止 |
| AmatsukazeServer/Server/Rest/RestApiHost.cs | `POST /api/trim/requeue` の追加 |
| AmatsukazeShared | 再投入リクエスト/レスポンスDTO追加、APIラッパー追加 |
| AmatsukazeServer/Server/EncodeServer.cs | `MakeAmatsukazeArgs` に `--resume-dir` 付与、`ClearWorkDirOnStart` の除外処理 |
| AmatsukazeServer/Server/TranscodeWorker.cs | ResumeDir存在チェックと受け渡し |
| AmatsukazeWebUI/Pages/TrimAdjust.razor | `RequeueWithProfile` を新APIに切替、クライアント側フォルダ削除の撤去 |

## 段階的実装

- Step 3a: `QueueItem.ResumeDir` + `MakeAmatsukazeArgs` + `ClearWorkDirOnStart` 除外
  (API未接続でもDBに手でResumeDirを入れれば動く状態)
- Step 3b: `/api/trim/requeue` とTrimAdjustServiceの再投入処理、WebUI切替
- 検証: サーバー経由でmode=cm→カット調整→再投入→再利用ログ確認(WebUI操作または REST直叩き)

## リスク・注意

- 再投入後にユーザーが同じ元タスクをもう一度カット調整→再投入した場合、同じResumeDirを
  複数Batchが共有し得る。2つ目以降の実行時にはフォルダがCLIに削除されている可能性があるが、
  CLI側フォールバックで完走はする(高速化だけ失われる)。初版はこの挙動を許容する。
- 別ドライブ/ネットワークパスのwork dirでも、ログ抽出パスは絶対パスなのでそのまま使える。
- ResumeDir付きBatchがリトライされる場合(サーバーのリトライ機構)、1回目の正常終了で
  フォルダが消えるとリトライは通常経路になる — 問題なし。
