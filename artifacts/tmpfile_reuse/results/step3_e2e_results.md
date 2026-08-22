# Step 3 サーバー連携 E2E検証結果 — 2026-07-10

素材: SAO OP2 MPEG2.ts / CM解析プロファイル「CM解析_一時保存」(NoRemoveTmp=true) / 再投入プロファイル「デフォルト」(chapter無効・cmoutmask=1)

## フロー(全てREST直叩き、サーバー経由)

1. `POST /api/queue/add` Mode=CMCheck → タスク192完了、一時フォルダ /tmp/amt5267317 に resume.dat 生成
2. `{src}.trim.avs` を作成(Trim(100,1000))
3. `POST /api/trim/requeue` (QueueItemId=192, Profile=デフォルト, RemoveSourceItem=true)
   → `{"queueItemId":193,"reuseTempDir":true}`
4. タスク193が `--resume-dir "/tmp/amt5267317"` 付きで起動

## 確認結果

| 項目 | 結果 |
|---|---|
| 再利用発動 | 合格。検証→マニフェスト読込→ストリーム情報読込→AMTSource再利用→CM解析再利用のログを確認(TS解析なし) |
| フィンガープリント緩和 | 合格。chapter設定不一致(CM解析=有効、デフォルト=無効)でも再利用が発動 |
| --trimavs適用 | 合格。「Trim情報入力」ログ確認 |
| 元CMタスク削除(RemoveSourceItem=true) | 合格。192はキューから削除、一時フォルダは即時削除されず193が所有 |
| 所有権ルール | 合格。193正常終了時にCLIデストラクタが一時フォルダを削除(NoRemoveTmp=false) |
| Mux・出力 | 合格。正常完走、mp4出力生成 |

## 初回E2E(緩和前)で確認したフォールバック動作

- chapter不一致でフィンガープリント検証が落ち、警告+通常経路で完走(安全側の動作を実環境で確認)
- フォールバック実行も指定一時フォルダを作業フォルダとして使用し、正常終了時に削除する
  (=キャッシュは1回の実行で消費される。設計どおりだが留意)

## 検証中に見つかった別件(要フォローアップ)

- **WebUIのプロファイル保存でフィールド欠落の疑い**: デフォルト.profileがWebUI保存(11:53)後に
  DisableMoveInputFile / EnableWebVTT / WhisperModel 等を失っていた。REST DTOが古い/不完全な
  可能性。ユーザー設定消失につながるため別途調査すべき。
  (暫定対応: デフォルト.profileにDisableMoveInputFile=trueを手で再挿入、バックアップは
  スクラッチパッドに保存)
