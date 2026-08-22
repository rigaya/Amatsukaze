# 一時ファイル再利用機能 検証計画

作成: 2026-07-10 / 対象: 03_design.md の Step 1・Step 2 実装

## テスト素材

- 主素材: `/mnt/x/Encoders/ソードアート・オンライン アリシゼーション War of Underworld OP2 MPEG2.ts`
  (2696フレーム・約1分半、ロゴ `SID1-1.lgd` あり、実測フォルダ /tmp/amt5253635 と同条件)
- 長尺素材: `/mnt/x/Encoders/MUSIC：S 欧州鉄道の旅・オランダ2 (BSフジ・182)_all_service.ts`
  (約3.7GB、全サービス収録)。V2の時間短縮・書き込み削減の効果計測と、
  サービス選択(-s)を伴う実運用相当の確認に使う。
- フォーマット変更あり(複数中間映像)の素材は用意困難のため、複数映像系は
  机上確認+コードレビューで担保する。

## 共通コマンド

mode=cm(ユーザー実測と同じ):

```
AmatsukazeCLI --mode cm -i <TS> -s 1 --drcs <drcs_map> -w /tmp \
  --chapter-exe chapter_exe --jls join_logo_scp --cmoutmask 2 --chapter \
  --jls-cmd <JL_標準.txt> --logo <SID1-1.lgd> --no-remove-tmp
```

mode=ts は上記に加えエンコーダ指定。出力一致比較のため決定性のある設定を使う:
`x264 --preset ultrafast --threads 1`(+CRF固定)。
trimファイルはmode=cmの `trim0.avs` を基に手で編集して作る(例: `Trim(0,1000)`)。

## V1: Step 1(マニフェスト保存)の検証

| # | 内容 | 合格条件 |
|---|---|---|
| V1-1 | mode=cm --no-remove-tmp 実行 | `<tmp>/streaminfo.dat` と `<tmp>/resume.dat` が生成される |
| V1-2 | 同上の既存成果物比較 | trim0.avs / jls0.txt / chapter系 / ログ内容が実装前と同一(マニフェスト保存以外の差分なし) |
| V1-3 | 通常mode=ts(再利用なし)のリグレッション | 実装前後で出力(映像・音声・チャプター・JSON)一致 |
| V1-4 | streaminfo.dat の妥当性 | deserializeして読めること(Step 2実装後はV2で兼ねる) |

## V2: Step 2(復元経路)の正常系

基準系と再利用系で出力一致を確認する。エンコードが決定的な設定にすること。

- 基準系: mode=cm(trim取得用)→ 通常 mode=ts `--trimavs <edited-trim>`(フル実行)
- 再利用系: mode=cm `--no-remove-tmp` → mode=ts `--resume-dir <tmp> --trimavs <edited-trim>`

| # | 比較項目 | 合格条件 |
|---|---|---|
| V2-1 | 出力映像 | フレーム数・トリム位置一致(可能ならファイルハッシュ一致) |
| V2-2 | 出力音声 | 同上 |
| V2-3 | チャプター出力 | 内容一致 |
| V2-4 | 出力JSON | srcfilesize等のスカラ含め一致 |
| V2-5 | ログ | 再利用経路に入ったこと・スキップした処理が明示されている |
| V2-6 | 書き込み削減 | 再利用実行中に i0.mpg / audio.* / amts0.dat の再書き込みが発生しない(mtime不変で確認) |
| V2-7 | 時間短縮 | TS解析・CM解析時間ぶんの短縮をログの時間表示で確認 |

## V3: フォールバック系(すべて「警告ログ+通常経路で完走」が合格条件)

| # | 状況の作り方 |
|---|---|
| V3-1 | resume.dat を削除して --resume-dir 実行 |
| V3-2 | streaminfo.dat を削除して実行 |
| V3-3 | 別のTSファイルを -i に指定(入力TS指紋不一致) |
| V3-4 | 入力TSにtouchでmtime変更(指紋不一致) |
| V3-5 | 設定変更(例: --splitsub 追加、デコーダ変更)でフィンガープリント不一致 |
| V3-6 | i0.mpg を削除(必要ファイル欠損) |
| V3-7 | resume.dat の先頭バイトを壊す(パース失敗) |

## V4: エッジケース

| # | 内容 | 確認点 |
|---|---|---|
| V4-1 | div0.txt が空(主素材で発生済み) | divs正規化済み保存で復元が正常 |
| V4-2 | --chapter なしでcm実行→再利用 | trims空=解析なしとして復元、チャプター出力なし |
| V4-3 | ロゴ指定なし(--logoなし・ロゴ消し無効) | logopath空で復元、ロゴ消しなしで完走 |
| V4-4 | --trimavs なしで --resume-dir のみ | 保存済みtrimsのままエンコード(CM解析結果の再利用) |
| V4-5 | 再利用実行を2回連続 | 一時フォルダが破壊されず2回目も成功(--no-remove-tmp併用) |

## V5: 検査の再現

| # | 内容 | 合格条件 |
|---|---|---|
| V5-1 | DRCS未マップ検査 | 未マップDRCSを含む素材(または drcs_map を空にして)cm→再利用tsで、通常tsと同じくNoDrcsMapExceptionで停止する |
| V5-2 | スクランブル検査 | 保存パケット数による検査ログが通常時と同等に出る(閾値超え素材がなければログ出力の確認のみ) |

## 実施要領

- 結果・ログは `artifacts/tmpfile_reuse/results/` に集約。
- 各ケースの再現コマンドをそのまま結果Markdownに残す。
- V1はStep 1レビュー後すぐ、V2以降はStep 2レビュー後に実施。
- サーバーは使わずAmatsukazeCLI直叩きで行う(Step 3のサーバー連携は別計画)。

## 既知の制約(この計画で扱わないもの)

- WebVTT / tsreplace 経路(raw.ts, tsreadex_dump.txt の再利用)は主素材では条件が
  揃わないため、コードレビューでの確認に留める。素材が用意できれば追加する。
- 複数中間映像素材が用意できない場合、V2系は単一映像のみ。
- Step 3(サーバー側からの --resume-dir 付与)は対象外。
