# Step 3 サーバー側連携 調査

調査日: 2026-07-10  
対象: `AmatsukazeServer`、`AmatsukazeShared`、`AmatsukazeWebUI`、`AmatsukazeGUI`（C#側）  
前提: C++側の一時ファイル再利用（`--resume-dir`）は `d1314ab` に含まれる。

## 結論

- カット調整で必要な一時フォルダは、**すでにサーバーがCLIログから取得している**。CLIから別のJSON・IPC通知を追加する必要はない。
- 現行の再投入は通常の `ProcMode.Batch` キュー追加であり、再利用先の一時フォルダをDTO・`QueueItem`・CLI引数へ渡す経路はない。Step 3ではこの経路に `ResumeDir`（名称は要検討）を追加する必要がある。
- 現行の「元CM解析タスクを削除する」設定では、新規Batchタスクの追加に成功した直後、**実行前に**元一時フォルダを削除する。`--resume-dir` を渡す実装ではこの順序のままでは再利用できない。
- `ClearWorkDirOnStart` が有効なら、ワーカープール開始時にwork path直下の `amt*` を一括削除する。カット調整待ち・再利用待ちのキャッシュも消えるため、寿命管理方針で扱う必要がある。

## 1. カット調整から再投入まで

### 入口

WPF GUIの「カット調整」はネイティブの別画面を持たず、WebUIの同じ画面をブラウザで開く薄い入口である。

| 段階 | 実装 | 内容 |
|---|---|---|
| GUIメニュー | `AmatsukazeGUI/Views/QueuePanel.xaml:1008-1010` | コンテキストメニューの「カット調整」が `OpenTrimAdjustCommand` を呼ぶ。 |
| GUI遷移 | `AmatsukazeGUI/ViewModels/QueueViewModel.cs:456-462`、`:772-807` | 完了済みBatch/AutoBatch/CMCheckのみ許可し、`/trim-adjust/{queueItemId}` をWebUIとして開く。 |
| WebUIセッション開始 | `AmatsukazeWebUI/Pages/TrimAdjust.razor:755-803` | `CreateTrimSessionAsync` にキューIDとプレビュー倍率を渡す。 |
| REST入口 | `AmatsukazeServer/Server/Rest/RestApiHost.cs:1779-1791` | `POST /api/trim/sessions` を `TrimAdjustService.TryCreateSession` に接続する。 |
| 一時ファイル利用 | `AmatsukazeServer/Server/Rest/TrimAdjustService.cs:440-503` | 対象キュー、ログ、一時フォルダ、`amts0.dat`を検査して `TrimAdjustSession` を作る。 |

`TrimAdjustService.TryCreateSession` は対象タスクのログを `ResolveTaskLogPathById` で取得し
（`TrimAdjustService.cs:446-458`）、`一時フォルダ: <path>` を抽出している
（`:650-691`）。`amts0.dat` の存在確認後に、AMTSourceベースのプレビューセッションを生成する
（`:461-503`）。編集結果は `{srcPath}.trim.avs` に保存される
（`TrimAdjustService.cs:603-648`、特に `:630-641`）。

### 再投入

WebUIの `RequeueWithProfile` が編集内容を必要に応じて保存後、元入力・出力先・選択プロファイル・優先度・タグを使って通常のキュー追加APIを呼ぶ。

| 段階 | 根拠 | 内容 |
|---|---|---|
| Trim保存 | `AmatsukazeWebUI/Pages/TrimAdjust.razor:1623-1640` | 未保存の編集点を先に `SaveTrims` する。 |
| 新規Batch投入 | `TrimAdjust.razor:1668-1691` | `AddQueueRequest` を `Mode = ProcMode.Batch` で構築し、`Api.AddQueueAsync` を呼ぶ。ResumeDirは存在しない。 |
| REST変換 | `AmatsukazeServer/Server/Rest/RestApiHost.cs:440-506` | Shared DTOをサーバーDTOへ変換し、`server.AddQueue`へ渡す。ここでも一時フォルダ情報はコピーされない。 |
| QueueItem生成 | `AmatsukazeServer/Server/QueueManager.cs:487-513, 610-634` | TS情報を再読込して新しい`QueueItem`を作成・永続キューに登録する。 |
| CLI実行 | `AmatsukazeServer/Server/TranscodeWorker.cs:999-1029` | キューアイテムからCLI引数を作り、`ProcessStartInfo`で起動する。 |

`AddQueueRequest` のフィールドは入力、モード、出力、RequestId、追加時バッチ、タグのみである
（`AmatsukazeServer/Server/EncodeServerData.cs:1104-1125`）。作成される`QueueItem`にも一時フォルダまたは
再利用フォルダのフィールドはない（`:1175-1248`）。したがって、再投入タスクが待機・サーバー再起動をまたぐなら、
`ResumeDir`を`AddQueueRequest`だけでなく、最終的に永続化される`QueueItem`まで渡す必要がある。

## 2. CLI引数の組み立て

### mode分岐

CLI引数の中心は `EncodeServer.MakeAmatsukazeArgs` である
（`AmatsukazeServer/Server/EncodeServer.cs:1951-2453`）。

| キュー種別 | CLI引数 | 根拠 |
|---|---|---|
| `ProcMode.CMCheck` | `--mode cm` | `EncodeServer.cs:1972-1975` |
| `ProcMode.DrcsCheck` | `--mode drcs` | `EncodeServer.cs:1976-1979` |
| generic入力 | `--mode g` | `EncodeServer.cs:1980-1983` |
| Batch/AutoBatch/Test | `--mode`を追加しない | 同上。CLI側の既定値は`ts`（`Amatsukaze/AmatsukazeCLI.hpp:146-149, 239-243`）。 |

CMCheckではさらに `--chapter` が強制付与される
（`EncodeServer.cs:2040-2053`）。通常モードも共通して`-w`、chapter_exe、JLS、`--cmoutmask`を受ける
（`:2040-2047`）。

### `--no-remove-tmp` と `--trimavs`

- `--no-remove-tmp` はCMCheck専用ではなく、プロファイルの `NoRemoveTmp` が真のときに付く
  （`EncodeServer.cs:2408-2411`、データ定義は`EncodeServerData.cs:375-380`）。
  カット調整を開くにはCMタスクの`amts0.dat`が残っている必要があるため、現状はCM解析に使用する
  プロファイルでこの設定を有効にしていることが前提になる。
- `TranscodeWorker` は `{SrcPath}.trim.avs` が存在するかを確認し
  （`TranscodeWorker.cs:936-941`）、そのパスを`MakeAmatsukazeArgs`へ渡す（`:999-1005`）。
  引数組み立て側が非空なら `--trimavs` を追加する（`EncodeServer.cs:2427-2430`）。
- CLIは`--trimavs`を「通常のCMカット出力」で使用する入力として扱う
  （`Amatsukaze/AmatsukazeCLI.hpp:119-120`）。カット調整の保存先と現在のBatch再投入経路はここで接続されている。

Step 3では、通常Batchの上記引数へ `--resume-dir "<ResumeDir>"` を足すのが最小の挿入点である。
CMCheckのCLI起動時は、再利用可能なキャッシュを必ず残すために`--no-remove-tmp`を確実に付与する方針も必要になる。

## 3. サーバーが一時フォルダを知る手段

### 現状はCLIログ経由

CLIは設定ダンプで `一時フォルダ: <absolute path>` を標準出力へ出す
（`Amatsukaze/Amatsukaze/TranscodeSetting.cpp:1565-1578`）。サーバーはそのログを既に利用している。

- カット調整: `TrimAdjustService.ExtractTempDirFromLog` が正規表現で抽出
  （`TrimAdjustService.cs:414, 650-691`）。
- キュー削除時の後始末: `QueueManager.TryExtractTaskWorkDirFromLog` も同じ形式を抽出
  （`AmatsukazeServer/Server/QueueManager.cs:837-913`）。

出力JSONは出力ファイル、ロゴ、サイズ、エラー、CM解析有無、`trimavs`指定有無などを保存するが、
一時フォルダパスは含まない（`Amatsukaze/Amatsukaze/TranscodeManager.cpp:1736-1785`）。
`TranscodeWorker`のCLI起動は標準出力・標準エラーをリダイレクトするだけで
（`AmatsukazeServer/Server/TranscodeWorker.cs:1019-1029`）、一時フォルダ専用のIPC/DTOはない。

**見立て:** Step 3のためにCLIの通知形式を増やす必要はない。`TrimAdjustService`がすでに取得済みの
絶対パスを、再投入する`QueueItem.ResumeDir`のような永続フィールドへコピーし、`TranscodeWorker`から
`MakeAmatsukazeArgs`へ渡すのが適切である。ログ再走査だけで実行直前に取得する方法は、ログ削除・元CMタスク削除・
サーバー再起動で壊れやすいため避けるべきである。

## 4. 残置一時フォルダの寿命管理

### 現在存在する仕組み

| 契機 | 実装 | 挙動 |
|---|---|---|
| CLI通常終了 | `Amatsukaze/Amatsukaze/TranscodeSetting.cpp:577-610` | `noRemoveTmp_ == false`ならデストラクタで登録済み一時ファイルとディレクトリを削除する。`--resume-dir`は既存フォルダをそのまま`path_`に採用する。 |
| プロファイルで保持 | `EncodeServer.cs:2408-2411` | `NoRemoveTmp`が真なら`--no-remove-tmp`を付与し、CLI側では残置する。 |
| カット調整の明示削除 | `TrimAdjustService.cs:538-600`、RESTは`RestApiHost.cs:1856-1863` | CMCheck完了アイテムに限り、ログ抽出したフォルダを`Directory.Delete(tempDir, true)`で削除する。 |
| カット調整後の元CMタスク削除 | `TrimAdjust.razor:1660-1666, 1693-1715` | 設定`TrimAdjustDeleteCmTask`が真なら、新Batch追加成功後に一時フォルダを削除してから元CMアイテムをキューから削除する。 |
| キュー削除時の任意削除 | `QueueManager.cs:837-882, 970-998, 1181-1190` | `DeleteTaskWorkDirOnQueueRemove`が真の場合のみ、ログから得た絶対パスかつ`amt<数字>`、非リンクのフォルダを削除する。既定値はfalse（`EncodeServer.cs:1485-1497`）。 |
| ワーカー開始時の一括削除 | `EncodeServer.cs:349-371, 2464-2501` | `ClearWorkDirOnStart`が真ならwork path直下の`amt*`を全削除する。 |

`TrimAdjustSession`の5分TTLはネイティブデコードセッションを破棄するだけで、ディスク上の一時フォルダは削除しない
（`TrimAdjustService.cs:416-427, 730-756`）。そのため、上表の明示削除またはCLIの通常終了を除き、
残置フォルダに時間ベースの自動回収はない。

### Step 3で必要な変更方針

1. CMCheck完了後、カット調整に使う一時フォルダは再利用Batchタスクへ**所有権を移す**。新タスクの
   `QueueItem`にResumeDirを永続化し、`--resume-dir`としてCLIへ渡す。
2. 現在の`TrimAdjustDeleteCmTask`の即時`Directory.Delete`は、再利用対象では実行してはいけない。
   元CMアイテムをキューから削除すること自体は可能だが、フォルダ削除は再利用BatchのCLI終了後に委ねるか、
   再利用Batchの完了・失敗・取消を契機に行う必要がある。
3. 再利用Batchを`--no-remove-tmp`なしで起動すれば、CLIの既存デストラクタが正常終了時に同じResumeDirを
   削除する（`TranscodeSetting.cpp:583-592`）。ただしプロファイルの`NoRemoveTmp`が真、異常終了、取消では残る。
   これらを回収対象にする場合は、QueueManagerの既存の安全な`amt<数字>`検査を再利用するのがよい。
4. `ClearWorkDirOnStart`は待機中のResumeDirも無差別に削除する。再利用機能を有効にする運用では無効化するか、
   キュー中のResumeDirを除外するよう拡張しない限り、サーバー再起動後の再利用保証はできない。

## 実装範囲の見立て

最小でも以下を横断する。

- Shared/Server DTOまたは専用再投入API: ResumeDirを受け渡す。
- `QueueItem`: ResumeDirをDataContractで永続化する。
- `RestApiHost`と`QueueManager`: 新規BatchアイテムへResumeDirを移送する。既存の通常`/api/queue/add`に
  任意フィールドを増やすより、カット調整専用の再投入操作として入力CMアイテム・状態・一時フォルダをサーバー側で
  原子的に検証するAPIの方が、任意パス注入と削除競合を防げる。
- `TranscodeWorker`と`EncodeServer.MakeAmatsukazeArgs`: ResumeDirがある通常Batchだけへ`--resume-dir`を追加する。
- `TrimAdjust.razor`: `AddQueueAsync`後の即時一時フォルダ削除を、再利用時は行わない。

`--resume-dir`はC++側で入力TS・設定・中間ファイルを検証し、不一致時は通常処理へフォールバックする。
ただし寿命競合でフォルダ自体を消すと通常処理は可能でもカット調整の高速化は失われるため、サーバー側での
所有権管理は機能要件である。
