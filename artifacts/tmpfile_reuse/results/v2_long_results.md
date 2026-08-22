# V2-7 長尺素材での効果計測 — 2026-07-10

素材: `/mnt/x/Encoders/MUSIC：S 欧州鉄道の旅・オランダ2 (BSフジ・182)_all_service.ts`
(3.7GB、54455フレーム≒約30分、-s 182、SID182-1.lgd、チャプターあり)

## mode=cm(--no-remove-tmp)

- 全体: 42.4秒(TS解析 26.95秒 + ロゴ・CM解析 14.84秒 ほか)
- 一時フォルダ /tmp/amt5260542: **合計1.7GB**
  (i0.mpg 1.32GB / audio.wav 349MB / audio.dat 58MB / amts0.dat 5MB / streaminfo.dat 9.6MB / resume.dat 307B)

## mode=ts --resume-dir(x264 ultrafast crf23 threads1)

- 再利用経路に正常に入り(検証→復元ログ確認、TS解析なし)、エンコード86.3秒+Mux2.6秒で完走
- 出力: 793MB mp4、54455フレーム

## 効果

- **時間**: 共通処理 約42秒がほぼゼロに(再開検証+streaminfo読込は1秒未満〜数秒)
- **書き込み**: 一時ファイル約1.7GBの再書き込みを回避(streaminfo/resumeの再保存 約9.6MBのみ)
- 素材が長いほど・ビットレートが高いほど削減量は増える(i0.mpg・audio.wavはソース比例)
