# V2(復元経路 正常系)検証結果 — 2026-07-10

素材: SAO OP2 MPEG2.ts (2696フレーム) / エンコーダ: x264 --preset ultrafast --crf 23 --threads 1 / mp4出力
trim: `Trim(100,1000)` を --trimavs で指定

## 実行

- mode=cm(新ビルド、--no-remove-tmp): 一時フォルダ /tmp/amt5256263、resume.dat(303B)/streaminfo.dat(463KB)生成
- 基準系: 通常 mode=ts --trimavs(フル実行)
- 再利用系: mode=ts --resume-dir /tmp/amt5256263 --no-remove-tmp --trimavs

再現コマンドはスクラッチパッド v2/*.log 参照(基準系は -fmt 省略=mp4。
-fmt mkv はデフォルトmuxerがL-SMASHのため -m mkvmerge の指定が必要で今回は不使用)。

## 結果

| 項目 | 結果 |
|---|---|
| V2-1 映像 | 合格。ストリームMD5一致 (864c77e988fb905ac4de2d6ff48b0583)、901フレーム=Trim(100,1000)どおり |
| V2-2 音声 | 合格。ストリームMD5一致 (7f8d944ea87759c34f7f356120ff06d0) |
| V2-4 尺 | 合格。30.063367秒で一致 |
| V2-5 ログ | 合格。検証→マニフェスト読込→ストリーム情報読込→AMTSource再利用→CM解析再利用→(--no-remove-tmpによる)再保存を確認 |
| V2-6 書き込み削減 | 合格。i0.mpg/audio.dat/audio.wav/amts0.dat のmtime不変(再書き込みなし) |
| V2-7 時間短縮 | 合格。TS解析1.50秒+ロゴ・CM解析1.23秒(+自動ロゴ検出)がスキップされ、再利用系は全体10.4秒で完走 |

備考: mp4コンテナ全体のmd5は作成時刻メタデータで不一致になるため、ストリーム単位ハッシュで比較した。

## レビューで修正した点

- CMAnalyze::restore() の makeCMZones ガードを divs→trims 基準に修正(claude)。
  trims空でmakeCMZonesを呼ぶと全編CM扱いゾーンが生成されるため、divsでのガードは
  「チャプター無効+cm時--trimavs」でCMカット消失、無条件呼び出しは全編CM化のリスクがあった。
