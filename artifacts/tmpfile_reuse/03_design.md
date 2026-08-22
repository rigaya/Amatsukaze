# 一時ファイル再利用による共通処理スキップ 設計方針

作成: 2026-07-10 / 根拠: 01_tmpfiles.md, 02_internal_state.md, 実測フォルダ /tmp/amt5253635

## 目的とユースケース

1. `mode=cm --no-remove-tmp` でCM解析を実行(一時フォルダ残置)
2. trimファイルを作成(手動またはカット調整機能)
3. タスク再投入で `mode=ts --trimavs <trim>` を実行
4. このとき手順1の一時ファイルを再利用し、TS解析・CM解析などの共通処理をスキップして
   高速化とディスク書き込み削減を図る

### 効果見込み(実測フォルダ 2696フレーム・約30秒素材の例)

スキップされる書き込み: `i0.mpg` 217.6MB + `audio.wav` 16.5MB + `audio.dat` 2.7MB +
`amts0.dat` 246KB。実番組(30分〜)ではGB級になる。加えてTS解析・ロゴ/CM解析の計算時間を削減。

## 全体設計

### A. 再利用のトリガー: 新オプション `--resume-dir <path>`(仮称)

- 前回の一時フォルダ(例: `/tmp/amt5253635`)を明示指定する。自動検出はしない
  (一時フォルダ名は `amt<time由来コード>` で毎回変わるため。TranscodeSetting.cpp:596-615)。
- 指定時、`TempDirectory` は新規作成せず指定フォルダをそのまま `path_` として使う。
  **同一パスの継続使用は必須**: `amts<N>.avs` / `amts<N>.dat` が一時フォルダの絶対パスを
  内包しているため(実測: amts0.avs が `/tmp/amt5253635/amts0.dat` を参照)。
- 終了時の削除動作は従来どおり(`--no-remove-tmp` がなければ削除)。

### B. mode=cm 時の追加保存: 再利用マニフェスト

`isNoEncode` かつ一時フォルダを残す設定のとき、共通処理完了後(896行の分岐部)に
一時フォルダ内へ以下を書き出す。既存の `File::writeValue/writeArray` スタイルの
バイナリでよい。

1. `<tmp>/streaminfo.dat` — `StreamReformInfo::serialize()` そのまま(生入力6項目)。
   既存の `-streaminfo.dat`(出力先側パス)とは独立に、一時フォルダ内へ保存する。
2. `<tmp>/resume.dat` — 再利用マニフェスト:
   - フォーマットバージョン(int)
   - 入力TSの指紋: ファイルサイズ + 更新時刻
   - 設定フィンガープリント: serviceId指定値、splitSub、isEncodeAudio、
     フォーマット(tsreplace判定)、デコーダ設定、ロゴ指定・ロゴ消し関連
     (deserialize+prepare()の結果とCM解析結果の再現性に影響するもの)
   - splitter由来スカラ: 実 `serviceId`、`numTotalPackets`、`numScramblePackets`、
     `totalIntVideoSize`、`srcFileSize`
   - DRCS未マップ数: `ctx.getErrorCount(AMT_ERR_NO_DRCS_MAP)`
     (mode=cmでは794行の検査が `!isNoEncode` でスキップされるため、再利用実行時に
     検査を再現するのに必須)
   - 動画ごと(numVideoFiles個): `numFrames`、`logopath`(空可)、
     `trims`(**applyPmtCut適用後**のint列)、`divs`(readDivで正規化済みのint列)
   - ※ `cmzones` は `trims` から `makeCMZones()` で再導出できるため保存しない
   - ※ `sceneChanges` は保存しない(applyPmtCut適用済みのtrimsを保存するため再実行不要)

### C. mode=ts + --resume-dir 時の復元フロー

transcodeMain の共通処理部を以下のように分岐する。

1. **検証**: `resume.dat` を読み、バージョン・入力TS指紋・設定フィンガープリントを照合。
   不一致または必要ファイル欠損(streaminfo.dat / i<N>.mpg / audio.dat / audio.wav /
   amts<N>.dat / trim<N>.avs / jls<N>.txt 等)なら**警告ログを出して通常経路へフォールバック**。
2. **AMTSplitter::split() をスキップ**: `StreamReformInfo::deserialize()` で復元し、
   splitter由来スカラはマニフェストから取得。
3. **検査の再現**: スクランブル率検査(786-792行)は保存済みパケット数で、
   DRCS検査(794-799行)は保存済みエラーカウントで同等に実施。
4. **tsreadex(WebVTT時)**: `tsreadex_dump.txt` と `raw.ts` が存在すればスキップ。
   ただしmode=cm時に生成条件が揃っていない場合(設定変更)はフォールバック対象。
5. `reformInfo.prepare()` を**通常どおり実行**(導出状態はここで再計算)。
6. NicoJK は通常どおり実行(cm時には取得されていない)。
7. **SaveAMTSource をスキップ**: `amts<N>.dat` の存在を確認して再利用。
8. **CMAnalyze を復元経路で構築**: 新設の復元用初期化(マニフェスト注入)で
   `trims` / `divs` / `logopath` を設定し `makeCMZones()` を呼ぶ。
   `analyze()` / `applyPmtCut()` は呼ばない。
9. 主映像への `inputTrimAVS()`(--trimavs)は既存どおり動く(867-872行)。
10. `applyCMZones()`、`chapterMakers` 生成(既存 `jls<N>.txt` を読む)以降は既存のまま。

### D. 実装の主な変更点

| 箇所 | 変更 |
|---|---|
| TranscodeSetting.h/cpp | `--resume-dir` オプション追加、`TempDirectory` の既存フォルダ使用モード |
| TranscodeManager.cpp | 896行分岐部でのマニフェスト保存、共通処理部の再利用分岐 |
| StreamReform.h/cpp | 変更なし(既存 serialize/deserialize をそのまま使用) |
| CMAnalyze.h/cpp | 復元用初期化API追加(trims/divs/logopath注入 + makeCMZones) |
| AmatsukazeCLI / サーバー側 | オプションの受け渡し(カット調整タスク再投入時に付与) — 別段階 |

## エッジケース・注意

- `div<N>.txt` が0バイトのケースあり(実測)。保存するdivsは `readDiv` の正規化後
  (先頭0・末尾numFrames)なので復元側では問題にならないが、保存タイミングに注意。
- 複数中間映像(numVideoFiles > 1): 全映像分のCM状態を保存・復元する。--trimavsが
  上書きするのは主映像のみ。
- チャプター無効やロゴ消し無効など解析自体が走らなかった場合、trims等は空。
  「解析しなかった」状態もマニフェストで区別して復元する(空=解析なしとして扱う)。
- 自動ロゴ検出(`--auto-logo-detect`)の場合、選択された `.lgd` はロゴディレクトリ側の
  永続ファイル。マニフェストの `logopath` が指すファイルの存在確認を検証に含める。
- 再利用実行では入力TSそのものをほぼ読まない(WebVTT/tsreplace時のraw.ts除く)。
  入力TS指紋の照合が誤再利用防止の生命線。

## 段階的実装プラン

- **Step 1**: mode=cm時のマニフェスト保存のみ実装(既存動作への影響なし)。
  単体で動作確認: 実行後の一時フォルダにstreaminfo.dat/resume.datができること。
- **Step 2**: `--resume-dir` と復元経路を実装(opt-in)。
  検証: mode=cm→mode=ts(--resume-dir + --trimavs)の出力が、通常のmode=ts
  (--trimavs)の出力と一致すること。フォールバック動作の確認。
- **Step 3**: サーバー側(カット調整のタスク再投入)からのオプション付与。C#側の改修。

## 未解決・要確認

- サーバー側が一時フォルダのパスをどう保持・受け渡すか(Step 3で調査)。
- 一時フォルダの寿命管理(残置フォルダの掃除)はサーバー側の責務とするか。
