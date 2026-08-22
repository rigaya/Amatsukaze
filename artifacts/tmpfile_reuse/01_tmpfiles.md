# `mode=cm` の一時ファイル調査

## 調査範囲

`TranscodeManager.cpp` の `transcodeMain()` 開始から `isNoEncode` 分岐直前
（704--896行）を対象にした。以下のパス中の `<tmp>` は
`ConfigWrapper::CreateTempDir()` が初期化する一時ディレクトリである
（`TranscodeManager.cpp:713`, `TranscodeSetting.cpp:1603-1605`）。

`mode=cm` でもこの範囲は通常モードと共通で実行する。違いは、896行以降で
CM解析のみの場合に戻るため、音声出力、字幕作成、映像エンコード以降の生成物を
作らない点である（`TranscodeManager.cpp:896-915`）。各 `get*Path()` は
`regtmp()` 経由で削除対象へ登録される（`TranscodeSetting.cpp:1628-1630`）。

## TS 分離で必ず生成されるもの

| パス | 内容と生成根拠 | 生成条件 | `cm` / `ts` 差 |
|---|---|---|---|
| `<tmp>/audio.dat` | 分離済み圧縮音声の連結データ。`AMTSplitter` のコンストラクタで書込みオープンし（`TranscodeManager.cpp:179-185`）、各音声PESの符号化データを書き込む（`TranscodeManager.cpp:393-411`）。パスは `getAudioFilePath()`（`TranscodeSetting.cpp:1044-1046`）。 | 音声PESが存在する限り作成。 | 同一。通常モード後段でそのまま音声出力に使用（`TranscodeManager.cpp:968-1005`）。 |
| `<tmp>/audio.wav` | 分離済みデコードPCMの連結データ（WAVヘッダは持たない）。上記と同時に開き、PCMがある音声フレームを追記する（`TranscodeManager.cpp:184-185`, `393-411`）。パスは `getWaveFilePath()`（`TranscodeSetting.cpp:1048-1050`）。 | 音声PESが存在する限り作成。PCMを持たないフレームではデータは追記されない。 | 同一。通常モードで音声再エンコードとWhisper用wav作成に使用（`TranscodeManager.cpp:953-963`, `122-174`）。 |
| `<tmp>/i<N>.mpg` | 映像フォーマットが変わる単位に分けた中間MPEG-PS。映像フォーマット変更時に開き、PESを `psWriter` から出力する（`TranscodeManager.cpp:378-385`, `350-359`）。パスは `getIntVideoFilePath()`（`TranscodeSetting.cpp:1052-1054`）。 | 少なくとも1映像フォーマット。フォーマット変更ごとに増える。 | 同一。後述 `amts<N>.dat` とAviSynthフィルタ/エンコードの映像入力。 |
| `<tmp>/raw.ts` | 入力TSのバイト列コピー。`readAll()` が入力読取りと同時に書き出す（`TranscodeManager.cpp:232-251`）。パスは `getTmpRawTSPath()`（`TranscodeSetting.cpp:1169-1171`）。 | WebVTT有効、または出力形式が `FORMAT_TSREPLACE`（`TranscodeManager.cpp:237-242`）。 | `cm` でも設定が同じなら作る。通常モード後段のWebVTT/tsreplaceで使用する（`StreamReform.cpp:1460-1463`, `Muxer.cpp:266-268`）。 |

TS分離は `AMTSplitter::split()` が `readAll()` を呼び、フレーム・字幕・イベント・時刻
リストを `StreamReformInfo` に移す（`TranscodeManager.cpp:196-204`）。これらは単独の
一時ファイルではなく、必要なら後述の streaminfo に保存できる。

## TS 分離後、CM解析前に生成されるもの

| パス | 内容と生成根拠 | 生成条件 | `cm` / `ts` 差 |
|---|---|---|---|
| `<outVideoPath>-streaminfo.dat` | `StreamReformInfo` の生入力シリアライズ。出力先側であり `<tmp>` 配下ではない。書込は `TranscodeManager.cpp:758-760`、内容は `StreamReform.cpp:372-379`。 | `--dump-streaminfo` 相当の `isDumpStreamInfo()` が真。 | 同一。現在の保存タイミングは `prepare()` より前。 |
| `<tmp>/tsreadex_dump.txt` | `tsreadex -n -1 -r - raw.ts` の標準出力トレース。書込先生成は `TranscodeManager.cpp:762-782`、パスは `TranscodeSetting.cpp:1173-1175`。 | WebVTT有効。`raw.ts` も同時に必要。 | 同一。通常モード後段の `genWebVTT()` が最初のPCR抽出と `b24tovtt` の標準入力に使う（`StreamReform.cpp:1340-1481`）。 |
| `<tmp>/amts<N>.dat` | `AMTSource` の復元用バイナリ。中間映像パス、`audio.wav` パス、映像/音声フォーマット、映像/音声フレーム表、デコーダ設定を保存する（`TranscodeManager.cpp:826-840`, `AMTSource.cpp:741-757`）。パスは `TranscodeSetting.cpp:1125-1127`。 | 中間映像ファイルごと。 | 同一。CM解析と通常モードのフィルタが `AMTSource()` で読む（`CMAnalyze.cpp:254-274`, `FilteredSource.cpp:642`）。 |

## CM解析で生成されるもの

`numFrames >= 300` かつチャプター有効時にチャプター/CM解析を行う。一方、ロゴ消しが
有効ならチャプター解析の有無にかかわらずロゴ解析を行う
（`TranscodeManager.cpp:849-859`）。`mode=cm` と `mode=ts` の生成条件は同じである。

| パス | 内容と生成根拠 | 生成条件 |
|---|---|---|
| `<tmp>/amts<N>.avs` | `AMTSource(amts<N>.dat)` を読むロゴ解析用AVS。`makeAVSFile(..., false)` が作成（`CMAnalyze.cpp:53-56`, `254-275`）。パス定義は `TranscodeSetting.cpp:1133-1135`。 | `analyze()` が呼ばれた場合。ロゴ解析を実際に走らせなくても先に作られる。 |
| `<tmp>/amts<N>_8bit.avs` | chapter_exe用AVS。必要時にYV12変換・最大1080p化を含む（`CMAnalyze.cpp:263-270`）。パス定義は `TranscodeSetting.cpp:1129-1131`。 | 上と同じ。 |
| `<tmp>/logof<N>.txt` | 指定ロゴのフレーム別検出結果。選択ロゴの結果を `writeResult()` で出力（`CMAnalyze.cpp:390-410`）。パス定義は `TranscodeSetting.cpp:1137-1142`。 | ロゴ解析を実行し、`--logo` がある場合。ロゴが未検出でも通常は結果ファイルを書き出す。 |
| `<tmp>/logof<N>-<I>.txt` | 追加ロゴ消し用のフレーム別検出結果。`eraseLogoPath` の各要素に対して出力（`CMAnalyze.cpp:413-415`）。 | ロゴ解析を実行し、追加ロゴ消しが指定されている場合。 |
| `<tmp>/chapter_exe<N>.txt` | `chapter_exe` の `-o` 出力。無音/シーンチェンジ解析用の解析データで、実行引数を組み立てる箇所は `CMAnalyze.cpp:593-601`。パス定義は `TranscodeSetting.cpp:1144-1146`。 | チャプター/CM解析時（`CMAnalyze.cpp:100-105`）。 |
| `<tmp>/chapter_exe_o<N>.txt` | `chapter_exe` 標準出力。後に `SCPos` を読んで `sceneChanges` を作る（`CMAnalyze.cpp:603-612`, `692-718`）。パス定義は `TranscodeSetting.cpp:1148-1150`。 | チャプター/CM解析時。 |
| `<tmp>/trim<N>.avs` | `join_logo_scp` の `-o` 出力。先頭行の `Trim(start,end)` 群が残す区間を表し、補集合がCMゾーンになる（`CMAnalyze.cpp:614-627`, `651-671`, `737-755`）。パス定義は `TranscodeSetting.cpp:1152-1154`。 | チャプター/CM解析時。なお通常モードで指定する `--trimavs` は主映像のこの解析結果を後から上書きする（`TranscodeManager.cpp:867-875`, `CMAnalyze.cpp:223-237`）。 |
| `<tmp>/jls<N>.txt` | `join_logo_scp` の `-oscp` 出力。チャプター生成用詳細（JLS）で、`MakeChapter` が読む（`CMAnalyze.cpp:620-626`, `756-765`, `774-780`）。パス定義は `TranscodeSetting.cpp:1156-1158`。 | チャプター/CM解析時。 |
| `<tmp>/div<N>.txt` | `join_logo_scp` の `-odiv` 出力。分割点列。読取り時に先頭0・末尾`numFrames`へ正規化し、`StreamReformInfo::fileDivs_` に反映する（`CMAnalyze.cpp:620-626`, `673-690`; `StreamReform.cpp:177-185`）。パス定義は `TranscodeSetting.cpp:1160-1162`。 | チャプター/CM解析時。 |

### 自動ロゴ検出時のみの補助ファイル

`auto_logo_work_<N>.dat` と `auto_logo_<N>.tmp.lgd` を `<tmp>` 直下に作るが、成功・失敗・例外の全経路で削除する実装である
（`CMAnalyze.cpp:418-424`, `469`, `552`, `576`, `579-590`）。したがって再利用対象にはならない。
成功時にはロゴディレクトリへ `SID<serviceId>-auto-<timestamp>.lgd` を保存する
（`CMAnalyze.cpp:557-575`）。これは一時フォルダ外の永続ファイルである。

## 896行以降の `mode=cm` 固有処理

この調査範囲の直後で `--copy-trimavs` が有効なら、最長の `trim<N>.avs` を
`<入力元>.trim.avs` へコピーする（`TranscodeManager.cpp:31-63`, `896-899`）。
またチャプター出力を有効にすると、`<tmp>/chapter<V>-<F>-<D><CM>.txt` を作り得る
（`TranscodeManager.cpp:900-912`; パス定義 `TranscodeSetting.cpp:1164-1167`）。
これは896行までの共通処理ではないが、「CMのみ実行後に残るもの」として再利用設計では
保持対象に含める必要がある。

## 再利用の最小ファイル集合

TS再解析を省くには、少なくとも `i<N>.mpg`、`audio.dat`、`audio.wav`、
`amts<N>.dat` と、再構築可能な `StreamReformInfo` の入力を保持する必要がある。
WebVTT/tsreplaceなら `raw.ts`、WebVTTなら `tsreadex_dump.txt` も追加で必要になる。
CM解析も省くなら、主映像の外部 `--trimavs` に加え、複数映像を扱う場合は各
`trim<N>.avs` と `div<N>.txt`、チャプター出力を維持するなら `jls<N>.txt` が必要である。
現状の `CMAnalyze` には既存 `div<N>.txt` を読み込む公開APIがないため、ファイルを
残すだけでは再利用できず、復元APIまたは状態保存の追加が必要になる。
