# 共通処理の内部状態と再利用可否

## 結論

現在の `StreamReformInfo::serialize()` はTS解析の**生入力**だけを保存し、呼び出しも
`prepare()` 前である（`TranscodeManager.cpp:749-801`, `StreamReform.cpp:368-394`）。
よってこれを `deserialize()` しただけでは、通常モード後段が必要とする導出状態
（`prepare()` の結果、CMゾーン、出力ファイル表）は復元されない。

ただし、既存シリアライズを `deserialize()` 後に同じ設定で `prepare()` し直せば、
TS再解析なしで導出状態は再計算できる。CM解析をも省略するには、CM解析状態を別途
保存/復元する必要がある。特に `--trimavs` は主映像だけを上書きするため、複数中間
映像を持つ入力では主映像以外のCM状態も必要である（`TranscodeManager.cpp:822-881`）。

## `StreamReformInfo` の保存範囲

既存 `serialize()` が保存するのは次の6項目のみである
（`StreamReform.cpp:372-379`、逆変換は385-394行）。

| 保存済み項目 | 由来・後段での役割 | 評価 |
|---|---|---|
| `numVideoFile_` | 中間映像数。CM解析ループやJSON出力の回数に使用（`TranscodeManager.cpp:822`, `1338-1341`）。 | 既存streaminfoから復元可能。 |
| `videoFrameList_` | DTS順映像フレーム、ファイルオフセット、フォーマット。`prepare()` の時刻・分割・フィルタ入力構築の原料。 | 既存streaminfoから復元可能。保存コストは映像フレーム数に比例。 |
| `audioFrameList_` | 音声種別、符号化/PCMサイズ・オフセットを含む。`prepare()`、音声出力、Whisper wavで使用。 | 既存streaminfoから復元可能。保存コストは音声フレーム数に比例。 |
| `captionItemList_` | 字幕イベント。`genAudio()` 内の字幕出力用リスト生成の原料（`StreamReform.cpp:187-193`）。 | 既存streaminfoから復元可能。 |
| `streamEventList_` | PID変更等。`prepare()` 後のPMTカット判定で `getPidChangedList()` が読む（`StreamReform.cpp:205-223`）。 | 既存streaminfoから復元可能。 |
| `timeList_` | 放送時刻情報。最初のフレーム時刻の計算原料。NicoJK取得時刻にも使う（`TranscodeManager.cpp:803-810`）。 | 既存streaminfoから復元可能。 |

保存されない `StreamReformInfo` メンバは、`nicoJKList_`、`isEncodeAudio_`、
`isTsreplace_`、全てのPTS/フォーマット/フィルタ/出力マッピングの導出配列、CM分割
`fileDivs_`、出力キー/出力ファイル、音声オフセット、入出力時間である
（メンバ一覧 `StreamReform.h:270-325`）。このうち `prepare()` は設定値を保存して
`reformMain()` と `genWaveAudioStream()` を呼ぶ（`StreamReform.cpp:147-152`）。

## 状態ごとの分類

| 状態 | 896行までの構築根拠 | 通常モードでの使用 | 分類と必要な対処 |
|---|---|---|---|
| `StreamReformInfo` 生入力6項目 | `AMTSplitter::split()` がTS解析結果を移動（`TranscodeManager.cpp:196-204`, `749`）。 | `prepare()` から全後段の基礎。 | **既存一時外ファイルから復元可能**。`--dump-streaminfo` で作るstreaminfoをCM実行時に必ず残す。 |
| `StreamReformInfo` の導出状態（`filterFrameList_`、`filterAudioFrameList_`、`format_`、`fileDivs_` 以外、`outFiles_` 等） | `prepare()` が構築（`StreamReform.cpp:147-152`, `396`以降）。 | `SaveAMTSource`、CM解析、`genAudio`、音声/字幕/映像/Mux全体。 | **再計算必要だがTS再解析は不要**。deserialize後、同一の `splitSub`、音声エンコード、tsreplace設定で `prepare()` を必ず実行する。 |
| `fileDivs_` とフレームの `cmType` | `applyCMZones()` がCMゾーンと分割点を反映（`StreamReform.cpp:177-185`）。 | `genAudio()` が出力キー/音声/字幕/時間を組み立てる。 | **既存ファイルだけでは完全復元不可**。`trim<N>.avs` からゾーンは再構築可能だが、`div<N>.txt` を読む公開APIがない。CM状態を追加保存、または読込APIを追加する。 |
| `cmanalyze[N]` の `trims` と `cmzones` | `trim<N>.avs` を読んで構築（`CMAnalyze.cpp:121-133`, `651-671`, `737-755`）。主映像の `--trimavs` は `inputTrimAVS()` で上書き（`CMAnalyze.cpp:223-237`）。 | `applyCMZones()`、`MakeChapter`、`AMTFilterSource` のゾーン入力（`TranscodeManager.cpp:875`, `879`, `1162-1166`）。 | **追加保存で復元可能**。`trims`（int列）または `cmzones`（範囲列）を動画ごとに保存。外部`--trimavs`がある主映像は再読込で代替可。 |
| `cmanalyze[N]` の `divs` | `div<N>.txt` を読んで0/末尾を正規化（`CMAnalyze.cpp:673-690`）。 | `applyCMZones()` 経由で `fileDivs_` に反映し、後段の分割出力を決める。 | **追加保存で復元可能**。動画ごとのint列。既存`div<N>.txt`を復元APIで読んでもよい。 |
| `cmanalyze[N]` の `sceneChanges` | `chapter_exe_o<N>.txt` の `SCPos` を読む（`CMAnalyze.cpp:692-718`）。 | 通常経路ではPMTカット実行時だけ使用し、その後は不要。 | **`--trimavs` 経路では不要**。再利用時にPMTカットを再実行しないなら保存不要。再実行する設計なら追加保存またはchapter出力再読込。 |
| `cmanalyze[N]` の `logopath` | ロゴ解析結果から選択（`CMAnalyze.cpp:390-410`、自動検出は574-575）。 | `AMTFilterSource` のロゴ消し、出力JSON（`TranscodeManager.cpp:1162-1166`, `1338-1341`）。 | **追加保存で復元可能**。動画ごとのパス文字列だけなので定数サイズ。ロゴ消しが有効なら必須。`logof<N>.txt`だけからの復元APIは現状ない。 |
| `cmanalyze[N]` の `logoAnalysisDone` | `analyzeLogo()` が設定（`CMAnalyze.cpp:78-97`）。 | 解析を重複しないための実行中フラグだけ。 | **`--trimavs` 経路では不要**。再利用用に新規生成した`CMAnalyze`では使わない。 |
| `chapterMakers[N]` | `trims` と `jls<N>.txt` から生成（`TranscodeManager.cpp:877-880`, `CMAnalyze.cpp:756-765`）。 | `exec()` が一時chapterを生成（`TranscodeManager.cpp:928-934`）。 | **既存一時ファイルから復元可能**。`jls<N>.txt`とtrimsがあれば再生成する。追加保存は不要。 |
| `logoFound` | 各映像のフレーム数と `logopath` 有無を格納（`TranscodeManager.cpp:846-891`）。 | 896行前の「ロゴなし」例外判定だけ。 | **`--trimavs` 経路では不要**。再利用時に同じ検証を行うなら、`logopath`と既存の`reformInfo`から再計算可能。 |
| `serviceId` | splitterの実サービスID（`TranscodeManager.cpp:745-753`）。 | NicoJK、join_logo_scp環境変数、エンコーダ/バッチ、Mux前まで広く使用（`TranscodeManager.cpp:807-810`, `1175-1177`, `1241`）。 | **追加保存で復元可能**。`int` 1個。設定に明示serviceIdがあっても、実値はTS解析由来なので保存が安全。 |
| `numTotalPackets` / `numScramblePackets` | splitterのパケット計数（`TranscodeManager.cpp:751-753`）。 | 786--792行のスクランブル検査のみ。 | **`--trimavs` 経路では不要**（再利用でTS解析を飛ばすなら既に検査済み）。同じ検査結果を監査用に残すなら64bit整数2個を追加保存。 |
| `totalIntVideoSize` / `srcFileSize` | splitterの集計（`TranscodeManager.cpp:754-755`）。 | 成功時の出力JSONのみ（`TranscodeManager.cpp:1343-1344`）。 | **追加保存で復元可能**。64bit整数2個。JSONの正確性を保つ場合だけ必要。 |
| `eoInfo`、`encoderParallel` | 現行実行のエンコーダ設定から解析（`TranscodeManager.cpp:718-724`）。 | format検証、チャプター出力名、エンコード、Mux（例: `1200-1242`, `1301`）。 | **再計算必要**。再実行時のエンコーダ設定が正であり、保存値を再利用すると設定変更と矛盾する。 |
| `nicoJK` / `nicoOK` | `prepare()` 後、ネットワーク取得と整形で構築（`TranscodeManager.cpp:805-820`）。 | ASS生成・Mux。 | **再計算必要**。CMのみでは取得されないため既存のCM成果物にはない。 |
| `audioDiffInfo`、`keys`、`outFileInfo` | `genAudio()` 以降で生成（`TranscodeManager.cpp:917-926`）。 | 音声整合ログ、各出力のエンコード/Mux/JSON。 | **`--trimavs` 前の共通処理では未構築**。通常モードで必ず再計算する。 |

## 追加保存の推奨設計とコスト

最小限の追加メタデータを、CM実行の完了時に「再利用マニフェスト」として保存するのが
筋がよい。既存 `streaminfo` の形式を無条件に拡張すると旧ファイルとの互換性管理が
必要になるため、別ファイルにする方が安全である。

既存streaminfoのパスは `outVideoPath + "-streaminfo.dat"` であり
（`TranscodeSetting.cpp:1056-1058`）、二段目の出力先を変えると自動では見つからない。
再利用マニフェストにはstreaminfoへの絶対パスまたは一時ディレクトリからの相対パスを
記録する。さらに、誤った入力への再利用を防ぐため、入力TSのサイズ・更新時刻・可能なら
ハッシュと、サービスID、分離/デコーダ/字幕に影響する設定のフィンガープリントを持たせる
べきである。

| 保存対象 | おおよその保存コスト | 必要性 |
|---|---|---|
| 既存 `StreamReformInfo` 生入力 | 映像/音声フレーム数・字幕数・イベント数に比例。既存形式そのまま。 | TS解析省略に必須。 |
| `serviceId`、パケット数、ファイルサイズ | 数十バイト。 | serviceIdは必須。残りは検査/JSON精度を維持する場合。 |
| 動画ごとの `trims`、`cmzones`、`divs` | 区間・分割数に比例し通常は小さい。`int`中心なので数KB以下が見込まれる。 | CM解析省略と複数映像の正確な分割に必須。 |
| 動画ごとの `logopath` | パス文字列長だけ。 | ロゴ消し有効時に必須。 |
| チャプター用JLS | 既存 `jls<N>.txt` を保持すれば追加保存不要。サイズはシーン数に比例。 | チャプター出力を後段で行う場合に必要。 |

再利用時は、(1) 入力TS・設定・デコーダ設定と中間ファイル群の整合性を検証し、
(2) streaminfoをdeserializeして `prepare()` を実行し、(3) 保存済みCM状態を
`CMAnalyze` と `StreamReformInfo::applyCMZones()` に投入し、(4) 主映像だけは必要に
応じて `--trimavs` で上書き、の順が必要になる。現状はCM状態を外部から注入するAPIが
ないため、再利用実装には `CMAnalyze`/`StreamReformInfo` の小さな復元API追加が必要である。
