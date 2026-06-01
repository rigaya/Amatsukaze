# Amatsukaze Linux ニコニコ実況コメント対応 設計ドキュメント

## 1. 背景

### 1.1 ニコニコ実況の変遷

| 時期 | 出来事 |
|---|---|
| ～2020/12/15 | 旧ニコニコ実況（独立サービス） |
| 2020/12/15～ | 新ニコニコ実況（ニコ生ベースにリニューアル、チャンネル大幅縮小） |
| 2024/06/08～08/05 | サイバー攻撃でニコニコ全サービスダウン → NX-Jikkyo が避難所として誕生 |
| 2024/08/05～現在 | ニコニコ実況復旧 + NX-Jikkyo も公式にないchを補完する形で併存 |

### 1.2 現行Amatsukaze（Windows）のコメント取得方式

`NicoJK.cpp` の `makeASS_()` は以下3つのモードで動作する:

```
[CONV_ASS_LOG] NicoJKログ優先
  → NicoConvAss.exe を "-nicojk 1" 付きで呼び出し
  → NicoConvAss が内部で NicoJKLogCMD 経由でログ取得 → ASS生成

[CONV_ASS_XML] NicoJK18サーバー経由
  → NicoJK18Client.exe で nicojk18.sakura.ne.jp から XML取得 → 一時ファイル保存
  → NicoConvAss.exe にそのXMLを食わせて ASS生成

[CONV_ASS_TS] TSファイル直接
  → NicoConvAss.exe にTSを直接渡して ASS生成
```

いずれのモードでも **NicoConvAss.exe** が必須。NicoConvAss はクローズドソース（Windows用バイナリのみ配布）のため、Linux上ではそのまま動作しない。

### 1.3 Linux対応の方針

「**NicoConvAss + NicoJKLogCMD + tsukumijima過去ログAPI**」の組み合わせをベースとし、NicoConvAss/NicoJKLogCMD をオープンソースの仕組みで代替することで Linux 対応する。

## 2. 外部リソース

### 2.1 tsukumijima ニコニコ実況 過去ログ API

- URL: https://jikkyo.tsukumijima.net/
- ソースコード: https://github.com/tsukumijima/jikkyo-api (PHP/Laravel)
- 現在も安定稼働中（5分おきに全チャンネルの過去ログを自動収集）

#### エンドポイント

```
GET https://jikkyo.tsukumijima.net/api/kakolog/{実況ID}?starttime={unix}&endtime={unix}&format=xml
```

| パラメータ | 説明 |
|---|---|
| `{実況ID}` | jk1, jk2, ... jk211 等 |
| `starttime` | 取得開始時刻（UNIXタイムスタンプ） |
| `endtime` | 取得終了時刻（UNIXタイムスタンプ） |
| `format` | `xml` または `json` |

制限: 3日分を超える期間は一度に取得不可。

#### XMLレスポンス構造

```xml
<packet>
  <chat thread="スレッドID" no="コメント番号" vpos="再生位置(1/100秒)"
        date="投稿時刻(UNIX)" date_usec="マイクロ秒"
        mail="コマンド(184,色,位置等)" user_id="ユーザーID"
        premium="1" anonymity="1">コメント本文</chat>
  ...
</packet>
```

文字コード: UTF-8（BOMなし）、改行: LF

#### 対応チャンネル（主要なもの）

| 実況ID | チャンネル名 |
|---|---|
| jk1 | NHK総合 |
| jk2 | NHK Eテレ |
| jk4 | 日本テレビ |
| jk5 | テレビ朝日 |
| jk6 | TBSテレビ |
| jk7 | テレビ東京 |
| jk8 | フジテレビ |
| jk9 | TOKYO MX |
| jk101 | NHK BS |
| jk141 | BS日テレ |
| jk151 | BS朝日 |
| jk161 | BS-TBS |
| jk171 | BSテレ東 |
| jk181 | BSフジ |
| jk211 | BS11 |
| jk222 | BS12 |
| jk333 | AT-X |

（他多数、詳細は https://jikkyo.tsukumijima.net/ 参照）

### 2.2 danmaku2ass

- URL: https://github.com/m13253/danmaku2ass
- ライセンス: GPL-3.0
- 言語: Python
- 機能: ニコニコ動画/Bilibili/AcFun のコメントXML → ASS字幕変換
- ニコニコ形式XMLに対応済み

#### 主要オプション

| オプション | 説明 |
|---|---|
| `-s WIDTHxHEIGHT` | ステージサイズ（解像度） |
| `-fn FONT` | フォント名 |
| `-fs SIZE` | フォントサイズ |
| `-a ALPHA` | テキスト透明度 (0.0-1.0) |
| `-dm SECONDS` | スクロールコメント表示時間（デフォルト5秒） |
| `-ds SECONDS` | 静止コメント表示時間（デフォルト5秒） |
| `-o OUTPUT` | 出力ASSファイルパス |

### 2.3 serviceId → jk番号 マッピング

現行Amatsukazeでは `ch_sid.txt`（NicoConvAss付属）をタブ区切りで読み込んでいる:

```
NicoJK.cpp getJKNum():
  正規表現: ([^\t]+)\t([^\t]+)\t([^\t]+)\t([^\t]+)\t([^\t]+)
  列: jknum \t ? \t serviceId \t ? \t tvname
  → serviceId が一致する行の jknum と tvname を取得
```

Linux版ではこのマッピングテーブルを自前で保持する必要がある。

## 3. 現行コードの構造

### 3.1 呼び出しフロー

```
TranscodeManager.cpp:809
  nicoJK.makeASS(serviceId, startTime, srcDuration)
    └→ NicoJK::makeASS_()
        ├── [useNicoJKLog] nicoConvASS(CONV_ASS_LOG, startTime)
        ├── [nicojk18]     getJKNum() → getNicoJKXml() → nicoConvASS(CONV_ASS_XML, startTime)
        └── [else]         nicoConvASS(CONV_ASS_TS, startTime)
    └→ NicoJK::readASS()  // 生成されたASSファイルを読み込み

TranscodeManager.cpp:1029-1036
  NicoJKFormatter::generate() で最終ASS出力
```

### 3.2 NicoConvAss.exe の呼び出しインターフェース

```
NicoConvAss.exe -width {W} -height {H} -wfilename "{出力ASS}" -chapter 0 [-nicojk 1] -tx_starttime {unixtime} "{入力ファイル}"
```

| 引数 | 値 |
|---|---|
| `-width`, `-height` | 720p: 1280x720, 1080p: 1920x1080 |
| `-wfilename` | 出力ASSファイルパス |
| `-chapter` | 0（チャプター無効） |
| `-nicojk 1` | NicoJKログモード時のみ付与 |
| `-tx_starttime` | 番組開始時刻（UNIXタイムスタンプ） |
| 最後の引数 | TSファイルパス or XMLファイルパス |

### 3.3 NicoJK18Client のインターフェース

```
NicoJK18Client.exe jk{num} {starttime} {endtime} -x -f "{出力XMLパス}"
```

### 3.4 ASS出力バリエーション（NicoJKType）

| 型 | サフィックス | 解像度 | 説明 |
|---|---|---|---|
| `NICOJK_720S` | `-720S` | 1280x720 | 720p 通常 |
| `NICOJK_720T` | `-720T` | 1280x720 | 720p 半透明（makeT()で自動生成） |
| `NICOJK_1080S` | `-1080S` | 1920x1080 | 1080p 通常 |
| `NICOJK_1080T` | `-1080T` | 1920x1080 | 1080p 半透明（makeT()で自動生成） |

`makeT()` は通常版(S)のASSを読み込み、Style行の透明度を `&H00` → `&H70` に変更し、Outline有効・Shadow無効にしたバリアントを生成する。

### 3.5 readASS() が期待するASS構造

```
[Script Info]
...
[V4+ Styles]
Format: ...
Style: white,MS PGothic,28,...
...
[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,H:MM:SS.cc,H:MM:SS.cc,...
```

readASS() は `[Events]` セクションの `Dialogue:` 行を正規表現でパースし、start/end時刻を内部クロック (MPEG_CLOCK_HZ) に変換する。

## 4. 案A: Pythonスクリプトで NicoConvAss を代替

### 4.1 概要

NicoConvAss.exe + NicoJKLogCMD の代わりに、1本のPythonスクリプトで「API取得 → ASS変換」を行う。

```
Amatsukaze (C++)
  └→ 外部プロセス: python3 nicojk_ass.py <args>
       ├── tsukumijima API に HTTP GET → XML取得
       ├── danmaku2ass のロジックで XML → ASS変換
       └── ASSファイル出力
  └→ readASS() で既存通りASS読み込み
```

### 4.2 Pythonスクリプト インターフェース設計

```
python3 nicojk_ass.py \
  --channel jk1 \
  --starttime 1700000000 \
  --endtime   1700003600 \
  --width 1920 --height 1080 \
  --output /tmp/nicojk-1080S.ass \
  [--font "MS PGothic"] \
  [--fontsize 48] \
  [--duration 5.0] \
  [--opacity 0.8]
```

| 引数 | 必須 | 説明 |
|---|---|---|
| `--channel` | Yes | 実況チャンネルID（jk1, jk211等） |
| `--starttime` | Yes | 取得開始時刻（UNIXタイムスタンプ） |
| `--endtime` | Yes | 取得終了時刻（UNIXタイムスタンプ） |
| `--width` | Yes | 出力解像度 幅 |
| `--height` | Yes | 出力解像度 高さ |
| `--output` | Yes | 出力ASSファイルパス |
| `--font` | No | フォント名（デフォルト: MS PGothic） |
| `--fontsize` | No | フォントサイズ（デフォルト: 解像度に応じて自動） |
| `--duration` | No | スクロールコメント表示秒数（デフォルト: 5.0） |
| `--opacity` | No | 透明度（デフォルト: 0.8） |
| `--api-url` | No | 過去ログAPIのURL（デフォルト: https://jikkyo.tsukumijima.net） |

#### 終了コード

| コード | 意味 |
|---|---|
| 0 | 成功 |
| 1 | 一般エラー |
| 100 | チャンネルなし（NicoJK18Clientとの互換性のため） |

#### serviceId → channel 変換

Pythonスクリプト自身は `--channel jk1` のように jk番号を受け取る。serviceId→jk番号の変換は C++ 側（`getJKNum()`）が既に行っているため、スクリプト側では不要。

ただし、ch_sid.txt がNicoConvAss付属のクローズドなデータであるため、Linux版では **serviceId→jk番号のマッピングテーブルを自前で用意する必要がある**（TSの放送波から取得できる serviceId を実況チャンネルに紐付けるもの）。

### 4.3 Pythonスクリプトの内部構造

```python
# nicojk_ass.py 概略
#
# 1. コメント取得
#    - tsukumijima API に HTTP GET
#    - 3日制限があるため、期間が長い場合は分割リクエスト
#    - XMLレスポンスをパース
#
# 2. コメント → ASS 変換
#    - danmaku2ass のロジックを利用（ライブラリとしてimport or 組み込み）
#    - ニコニコ実況の mail 属性をパースしてコメント属性を決定:
#      - 色: white, red, pink, orange, yellow, green, cyan, blue, purple, ...
#      - サイズ: big, medium(default), small
#      - 位置: ue(上固定), shita(下固定), naka(スクロール, default)
#    - 衝突回避アルゴリズムで Y座標を決定
#    - ASS \move() タグでスクロールアニメーション生成
#
# 3. ASS出力
#    - [Script Info], [V4+ Styles], [Events] セクションを生成
#    - readASS() が期待するフォーマットに合わせる
```

### 4.4 Amatsukaze C++ 側の変更

#### NicoJK.cpp への変更

`makeASS_()` に Linux 用パスを追加する。概要:

```cpp
bool NicoJK::makeASS_(Stopwatch& sw, int serviceId, time_t startTime, int duration) {
#if defined(_WIN32) || defined(_WIN64)
    // 既存のWindows用フロー（NicoConvAss.exe / NicoJK18Client.exe）
    if (setting_.isUseNicoJKLog()) {
        if (nicoConvASS(CONV_ASS_LOG, startTime)) return true;
    }
    if (setting_.isNicoJK18Enabled()) {
        // ... 既存コード ...
    } else {
        // ... 既存コード ...
    }
#else
    // Linux用フロー: Pythonスクリプトで API取得 + ASS変換
    getJKNum(serviceId);
    if (jknum_ == -1) return false;
    return makeASSLinux(startTime, duration);
#endif
}
```

新規メソッド `makeASSLinux()` を追加:

```cpp
bool NicoJK::makeASSLinux(time_t startTime, int duration) {
    // nicojkmask に基づいて必要な解像度バリアントを生成
    // 各バリアントについて:
    //   1. Pythonスクリプトを外部プロセスとして呼び出し
    //      python3 nicojk_ass.py --channel jk{num} --starttime {t} --endtime {t+d}
    //                            --width {w} --height {h} --output {tmppath}
    //   2. 終了コード確認
    //   3. T(半透明)バリアントが必要なら makeT() で生成
}
```

#### TranscodeSetting への変更

- `nicoConvAssPath` は Linux では Pythonスクリプトのパスを保持するよう拡張
  - または、新たに `nicojkScriptPath` のような設定を追加
- `nicoConvChSidPath`（ch_sid.txt）は Linux でも同様に使用可能
  - serviceId → jk番号マッピングのデータファイルを同梱する

### 4.5 半透明バリアント (T) の生成

`makeT()` は既存の C++ コードがそのまま使える（ASSファイルのスタイル行を書き換えるだけの処理）。Pythonスクリプトは通常版 (S) のみ生成すればよい。

### 4.6 依存関係

- Python 3.6+
- 標準ライブラリのみ（urllib, xml.etree.ElementTree 等）
  - danmaku2ass は外部ライブラリ不要
  - requests は不要（urllib.request で十分）

### 4.7 ASS出力互換性の確認ポイント

danmaku2ass ベースの出力と NicoConvAss の出力で以下の差異が予想される。実用上問題がないか検証が必要:

| 項目 | NicoConvAss | danmaku2ass | 影響 |
|---|---|---|---|
| フォント | MS PGothic | 指定可能 | 合わせれば同等 |
| コメント速度 | 不明（ソース非公開） | -dm オプションで調整可能 | 要チューニング |
| 衝突回避 | 不明 | 独自アルゴリズム | 表示位置が異なる可能性 |
| 色の対応 | ニコニコ実況の全色対応 | ニコニコXMLのmail属性をパース | 要確認 |
| ASS Style定義 | 独自スタイル | danmaku2ass のスタイル | readASS()互換性の確認必要 |

**最も重要な確認**: readASS() が期待する `Dialogue: 0,H:MM:SS.cc,H:MM:SS.cc,...` 形式と一致するかどうか。danmaku2ass の ASS 出力フォーマットを確認し、必要に応じてスクリプト側で調整する。

## 5. 案B: C++ 完全内蔵化

### 5.1 概要

案AのPythonスクリプトが安定して動作し、出力品質も十分と確認された後、外部Python依存を排除するためにC++で再実装する。

```
Amatsukaze (C++)
  └→ NicoJK.cpp 内で直接:
       ├── HTTP GET で tsukumijima API を叩く (libcurl)
       ├── XMLパース (既存のXMLパーサ or tinyxml2)
       ├── コメント → ASS変換 (danmaku2ass 参考の C++ 実装)
       └── ASSファイル出力
  └→ readASS() で読み込み
```

### 5.2 追加依存

- libcurl（HTTP取得用）
  - Linux ではほぼ標準でインストール済み
  - 既にAmatsukazeが使っている場合はそのまま利用

### 5.3 実装すべきもの

| コンポーネント | 作業量 | 備考 |
|---|---|---|
| HTTP GETクライアント | 小 | libcurl の簡単なラッパー |
| XMLパーサ（chat要素抽出） | 小 | 既存パーサ利用 or tinyxml2 |
| mail属性パーサ | 小 | 色・サイズ・位置の解析 |
| 衝突回避アルゴリズム | 中 | danmaku2ass の Python 実装を参考にC++化 |
| ASS生成（Style + Dialogue） | 中 | readASS() が期待するフォーマットに合わせる |

### 5.4 移行判断基準

案A → 案B への移行は以下をすべて満たす場合に実施:

1. 案Aで出力品質が十分と確認された（Windows版NicoConvAssとの比較検証完了）
2. Python依存を排除する強い理由がある（Docker軽量化等）
3. 案Aのロジックが安定し、C++化のリスクが低いと判断できる

Python依存がLinux環境で問題にならない場合、案Aのまま運用を継続してよい。

## 6. 作業手順

### Phase 1: Pythonスクリプト作成（案A）

1. **serviceId → jk番号マッピングデータの作成**
   - 既存の ch_sid.txt 相当のデータを調査・整理
   - Amatsukaze のデフォルトデータとして同梱

2. **nicojk_ass.py の実装**
   - tsukumijima API からのコメント取得
   - danmaku2ass ベースの ASS 変換ロジック組み込み
   - readASS() 互換の ASS フォーマット出力
   - 単体テスト（特定チャンネル・時間帯でASS生成を確認）

3. **ASS出力の検証**
   - Windows版 NicoConvAss の出力と比較
   - readASS() で正しくパースできることの確認
   - 実際の動画プレイヤー（mpv 等）での表示確認

### Phase 2: Amatsukaze C++ 側統合

4. **NicoJK.cpp の改修**
   - Linux 用の `makeASSLinux()` メソッド追加
   - Pythonスクリプト呼び出しロジック
   - 終了コードのハンドリング

5. **設定・UIの対応**
   - NicoConvASSPath の代わりにPythonスクリプトパスを設定可能に
   - WebUI での設定項目追加（必要に応じて）

6. **結合テスト**
   - 実際のTSファイルを使ったエンコードでコメント付き出力を確認

### Phase 3: 案Bへの移行（オプション）

7. 案Aが安定稼働した後、必要であればC++化を実施
