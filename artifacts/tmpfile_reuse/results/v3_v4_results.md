# V3/V4 検証結果

実施日: 2026-07-10  
実行ディレクトリ: `/home/rigaya/dev/Amatsukaze`  
素材: `/mnt/x/Encoders/ソードアート・オンライン アリシゼーション War of Underworld OP2 MPEG2.ts` (2697フレーム)  
エンコーダ: `x264 --preset ultrafast --crf 23 --threads 1 --quiet`  
出力: Matroska (`mkvmerge`)

## 共通準備

V3-1〜7およびV4-2/3では、次の `mode=cm` 実行で作成したロゴなし・チャプターなしの
一時フォルダ `/tmp/amt5255674` をコピーして用いた。元の一時フォルダは変更していない。

```sh
build/exe_files/AmatsukazeCLI --mode cm -i '/mnt/x/Encoders/ソードアート・オンライン アリシゼーション War of Underworld OP2 MPEG2.ts' --no-delogo --no-remove-tmp -w /tmp
```

V4-1/4では、V2で作成した `/tmp/amt5256263` をコピーして使用した。同フォルダ自体には
書き込み・削除をしていない。以下で `CMD` は全ケース共通のエンコード・mux指定である。

```sh
CMD="-fmt mkv -e build/exe_files/x264 -eo '--preset ultrafast --crf 23 --threads 1 --quiet' -m /usr/bin/mkvmerge"
```

## V3: フォールバック

全7ケースで、再利用を中止する警告の後に `TS解析完了` と `Mux完了` を確認し、出力ファイルが
生成された。したがって、いずれも通常経路へのフォールバックとして合格である。

| ケース | 再現手順 | 警告ログ | 結果 |
|---|---|---|---|
| V3-1 | `cp -a --reflink=auto /tmp/amt5255674 /tmp/amt_v3_1`、`rm /tmp/amt_v3_1/resume.dat` 後、`build/exe_files/AmatsukazeCLI --mode ts -i '<主素材>' -o /tmp/v3_1.mkv --no-delogo --resume-dir /tmp/amt_v3_1 --no-remove-tmp $CMD` | `再開情報が見つからないため通常処理へ戻ります` | `/tmp/v3_1.mkv` (143270417B) を生成。通常経路で完走。 |
| V3-2 | `cp -a --reflink=auto /tmp/amt5255674 /tmp/amt_v3_2`、`rm /tmp/amt_v3_2/streaminfo.dat` 後、`build/exe_files/AmatsukazeCLI --mode ts -i '<主素材>' -o /tmp/v3_2.mkv --no-delogo --resume-dir /tmp/amt_v3_2 --no-remove-tmp $CMD` | `再開情報が見つからないため通常処理へ戻ります` | `TS解析完了: 1.65秒`、`Mux完了: 0.39秒`。出力143270417B。 |
| V3-3 | `cp -a --reflink=auto /tmp/amt5255674 /tmp/amt_v3_3` 後、`build/exe_files/AmatsukazeCLI --mode ts -i /mnt/x/Encoders/1.ts -o /tmp/v3_3.mkv --no-delogo --resume-dir /tmp/amt_v3_3 --no-remove-tmp $CMD` | `入力TSのサイズまたは更新時刻が一致しません。通常処理へ戻ります` | `TS解析完了: 1.14秒`、`Mux完了: 0.20秒`。出力68110560B。 |
| V3-4 | `cp --reflink=auto '<主素材>' /tmp/v3_4_input.ts`、`touch /tmp/v3_4_input.ts`、`cp -a --reflink=auto /tmp/amt5255674 /tmp/amt_v3_4` 後、`build/exe_files/AmatsukazeCLI --mode ts -i /tmp/v3_4_input.ts -o /tmp/v3_4.mkv --no-delogo --resume-dir /tmp/amt_v3_4 --no-remove-tmp $CMD` | `入力TSのサイズまたは更新時刻が一致しません。通常処理へ戻ります` | 元素材のmtimeは変更せず、コピーのみtouch。`TS解析完了: 0.57秒`、`Mux完了: 0.39秒`。出力143270417B。 |
| V3-5 | `cp -a --reflink=auto /tmp/amt5255674 /tmp/amt_v3_5` 後、`build/exe_files/AmatsukazeCLI --mode ts -i '<主素材>' -o /tmp/v3_5.mkv --no-delogo --splitsub --resume-dir /tmp/amt_v3_5 --no-remove-tmp $CMD` | `再開情報と再実行時の設定が一致しません。通常処理へ戻ります` | `TS解析完了: 1.73秒`、`Mux完了: 0.38秒`。出力143270417B。 |
| V3-6 | `cp -a --reflink=auto /tmp/amt5255674 /tmp/amt_v3_6`、`rm /tmp/amt_v3_6/i0.mpg` 後、`build/exe_files/AmatsukazeCLI --mode ts -i '<主素材>' -o /tmp/v3_6.mkv --no-delogo --resume-dir /tmp/amt_v3_6 --no-remove-tmp $CMD` | `再開に必要な映像一時ファイルがありません: 0。通常処理へ戻ります` | `TS解析完了: 1.69秒`、`Mux完了: 0.38秒`。出力143270417B。 |
| V3-7 | `cp -a --reflink=auto /tmp/amt5255674 /tmp/amt_v3_7`、`dd if=/dev/zero of=/tmp/amt_v3_7/resume.dat bs=1 count=1 conv=notrunc` 後、`build/exe_files/AmatsukazeCLI --mode ts -i '<主素材>' -o /tmp/v3_7.mkv --no-delogo --resume-dir /tmp/amt_v3_7 --no-remove-tmp $CMD` | `再開情報の検証に失敗したため通常処理へ戻ります: Exception thrown at TranscodeManager.cpp:182` | `TS解析完了: 1.82秒`、`Mux完了: 0.38秒`。出力143270417B。 |

注: 表中の `<主素材>` は本書先頭の素材パスであり、実行時にはシングルクォートで囲んだ。

## V4: エッジケース

V4-1〜4はすべて警告なしで再利用経路に入り、`TS解析完了` は出ずに
`ソースファイル読み込み用データを再利用します` と `ロゴ・CM解析結果を再利用します` を確認した。

| ケース | 再現手順 | 確認結果 |
|---|---|---|
| V4-1 | `cp -a --reflink=auto /tmp/amt5256263 /tmp/amt_v4_1` (`wc -c /tmp/amt_v4_1/div0.txt` は `0`) 後、`build/exe_files/AmatsukazeCLI --mode ts -i '<主素材>' -s 1 --chapter --logo /home/rigaya/dev/Amatsukaze/build/logo/SID1-1.lgd --cmoutmask 2 -o /tmp/v4_1.mkv --resume-dir /tmp/amt_v4_1 --no-remove-tmp $CMD` | 空の `div0.txt` でもマニフェストの正規化済みdivsを復元して完走。チャプター生成を実行し、`フィルタ出力: 901フレーム`、`Mux完了: 0.13秒`。 |
| V4-2 | `cp -a --reflink=auto /tmp/amt5255674 /tmp/amt_v4_2` 後、`build/exe_files/AmatsukazeCLI --mode ts -i '<主素材>' -o /tmp/v4_2.mkv --no-delogo --resume-dir /tmp/amt_v4_2 --no-remove-tmp $CMD` | `チャプター解析: 無効`。trims空の再開情報を正常に復元し、全2697フレームを出力。`Mux完了: 0.37秒`。 |
| V4-3 | `cp -a --reflink=auto /tmp/amt5255674 /tmp/amt_v4_3` 後、`build/exe_files/AmatsukazeCLI --mode ts -i '<主素材>' -o /tmp/v4_3.mkv --no-delogo --resume-dir /tmp/amt_v4_3 --no-remove-tmp $CMD` | `--logo`なし、`--no-delogo`あり。保存後の `resume.dat` に可読なロゴパスはなく、空のlogoPathを復元して正常完走。`Mux完了: 0.37秒`。 |
| V4-4 | `cp -a --reflink=auto /tmp/amt5256263 /tmp/amt_v4_4` 後、`build/exe_files/AmatsukazeCLI --mode ts -i '<主素材>' -s 1 --chapter --logo /home/rigaya/dev/Amatsukaze/build/logo/SID1-1.lgd --cmoutmask 2 -o /tmp/v4_4.mkv --resume-dir /tmp/amt_v4_4 --no-remove-tmp $CMD` | `--trimavs`を指定せず、保存済みtrimsが適用された。`フィルタ出力: 901フレーム`、`Mux完了: 0.12秒`。 |

V4-5は今回の依頼対象がV4の4ケースだったため未実施。

## 後処理

`/tmp/amt5256263` は保存したまま、`/tmp/amt_v3_*` と `/tmp/amt_v4_*` の検証用コピーだけを削除した。
