# EDCB連携・実行コンテキスト問題のWeb生情報収集（第1回スイープ）

日付: 2026-07-10 / 担当: 結愛
目的: EDCB録画後バッチ×Amatsukaze連携の障害事例を収集し、理論マトリクスとの整合を確認する（P2-3b）。

## 収集した事例と一次ソース

### 1. サービス起動→サーバーがサービスコンテキストを継承

- 症状: EDCBがサービスで動作、EpgTimer/AmatsukazeServer未起動の状態で録画後バッチが走ると、
  AmatsukazeServerCLIがEDCBのコンテキストで起動し、AviSynthプラグインがエラーを吐くことがある。
- ソース: 本家README（既知）、複数のまとめ記事で言及。
- 対処: AmatsukazeServerをユーザーセッションで事前起動（README記載の運用回避策）。

### 2. EDCBサービスのアカウントは2種類ある（理論の修正点）

- EpgTimerSrv_Install.bat は LocalSystem と LocalService の2択を提供する。
  https://github.com/xtne6f/EDCB/blob/work-plus-s/ini/EpgTimerSrv_Install.bat
- LocalService の場合、録画フォルダへの書き込みすらACL付与が必要になる事例:
  https://melog.info/archives/2022/09/25/6746
- 含意: マトリクスの「サービス化」行は LocalSystem / LocalService に分割が必要。
  LocalServiceはネットワーク共有アクセスがさらに制限的（匿名相当）。

### 3. ファイアウォール初回ブロック→「録画後に何も起きない」

- 症状: EDCB経由で初めてAmatsukazeServer.batが起動されるとファイアウォールにブロックされ、
  録画後に何も実行されないように見える。
- 対処: 事前に一度手動起動して許可しておく。
- ソース: https://enctools.com/amatsukaze-bat-run/

### 4. 文字コード（UTF-8/ANSI）

- 症状: EDCBの録画後バッチで文字化けが発生し、ps1経由で回避した事例。
  https://nextaltair.hatenablog.com/entry/edcb_auto_encode_batch_file_utf8_problem_solution_with_ps1
- 関連: rigaya/Amatsukaze open issue #15（Windows 11「Unicode UTF-8を使用する」でCLI出力文字化け）。
  診断ログにANSIコードページを含めた（65001なら当該設定が有効）。

### 5. カレントディレクトリ/AmatsukazeRoot問題（新発見の軸）

- 症状: AmatsukazeAddTaskをAmatsukazeルート以外のカレントディレクトリから実行すると、
  サーバーの成り行き起動が失敗し「サーバのリクエスト受理を確認できませんでした」を繰り返す。
- 原因: AddTaskは生成バッチからの利用（ルート=カレント）前提だった。
- 対処: nekopanda氏がexeの1つ上をルートとして起動するよう修正済み（commit 2b0cf3e）。
  ただし配置を変えた環境では再発しうる。
- ソース: https://github.com/nekopanda/Amatsukaze/issues/5

### 6. UAC/管理者権限

- 「管理者権限で実行してみて」という一般的アドバイスが散見される（OKWAVE等）。
  タスクスケジューラの「最上位の特権で実行する」での自動起動が定番回避策。
- ソース: https://okwave.jp/qa/q10172296.html ほか一般記事。
- 注意: 管理者昇格したプロセスから見えるドライブレターは非昇格セッションと別管理
  （EnableLinkedConnections未設定時）。「管理者で実行したらネットワークドライブが消えた」
  は昇格に起因する場合がある。

### 7. 多重起動検出とSYSTEMサーバー不可視

- サービスコンテキストで起動済みのサーバーはユーザーから見えず、「起動しない」と誤認される。
  READMEに「起動しないと思ったらクライアントで接続してみてください」の記載あり。

## 理論マトリクスとの整合確認

| 軸1: バッチ実行コンテキスト | 軸2: 投入経路 | 予想される問題 | Web事例 |
|---|---|---|---|
| ユーザー(EpgTimerSrv通常起動) | Server事前起動 | なし（推奨構成） | 問題報告なし=整合 |
| ユーザー | AddTask成り行き起動 | カレントディレクトリ問題(#5)、ファイアウォール | 事例あり=整合 |
| LocalSystem(サービス) | 成り行き起動 | AviSynthエラー、ネットワークドライブ不可視、サーバー不可視 | READMEと事例=整合 |
| LocalService(サービス) | 成り行き起動 | 上記+録画フォルダACL、共有アクセス不可 | Melog事例=整合（Amatsukaze直接の報告は未発見） |
| 管理者昇格(UAC) | 任意 | 昇格によるドライブレター分離 | 一般知識と整合（Amatsukaze直接の報告は未発見） |

結論: 理論と矛盾する事例は見つからなかった。ただし「LocalService×Amatsukaze」「UAC昇格×ドライブ分離」の
Amatsukaze固有の直接報告は未発見のため、doc記載時は「起こりうる」として書き、断定しない。

## 未実施・残タスク

- 5chアーカイブ(過去スレ)の深掘りは未実施（2ch.scミラーの自動抽出は精度が低く手動確認が必要）
- X(Twitter)の報告検索は未実施
- スポット検証（schtasks /ru SYSTEMでの簡易再現）はP2-3cで実施予定
