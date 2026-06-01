# Amatsukaze NicoJK ASS Generator for Linux
# Copyright (c) 2017-2019 Nekopanda
# This software is released under the MIT License.
# http://opensource.org/licenses/mit-license.php

"""
ニコニコ実況 過去ログ取得 & ASS字幕変換スクリプト

tsukumijima 過去ログ API からコメントXMLを取得し、
readASS() 互換のASS字幕ファイルを生成する。

使用方法:
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

終了コード:
  0   成功
  1   一般エラー
  100 チャンネルなし（C++側が100を特別扱いする）
"""

import argparse
import sys
import urllib.request
import urllib.error
import xml.etree.ElementTree as ET

# ------------------------------------------------------------------ #
# 定数定義
# ------------------------------------------------------------------ #

# API のベースURL
DEFAULT_API_BASE = "https://jikkyo.tsukumijima.net"

# 1リクエストあたりの最大期間（秒）。tsukumijima API の3日制限に合わせる
MAX_FETCH_SECONDS = 3 * 24 * 3600

# デフォルトフォント名
DEFAULT_FONT = "MS PGothic"

# デフォルトフォントサイズ（未指定時は解像度から自動算出）
DEFAULT_FONTSIZE_BASE = 36  # 720p 相当の基準サイズ

# スクロールコメントのデフォルト表示秒数
DEFAULT_DURATION = 5.0

# デフォルト不透明度（0.0〜1.0）
DEFAULT_OPACITY = 0.8

# 固定コメントのデフォルト表示秒数
FIXED_COMMENT_DURATION = 4.0

# 終了コード定数
EXIT_SUCCESS = 0
EXIT_ERROR = 1
EXIT_CHANNEL_NOT_FOUND = 100

# フォントサイズ倍率
FONTSIZE_SCALE_BIG = 1.44
FONTSIZE_SCALE_SMALL = 0.64

# ニコニコ実況の色名 → RGB 16進数 マッピング（ニコニコ公式色）
# ASS形式は BGR 順なので変換時に注意
NICOJK_COLORS = {
    "white":          0xFFFFFF,
    "red":            0xFF0000,
    "pink":           0xFF8080,
    "orange":         0xFFC000,
    "yellow":         0xFFFF00,
    "green":          0x00FF00,
    "cyan":           0x00FFFF,
    "blue":           0x0000FF,
    "purple":         0xC000FF,
    "black":          0x000000,
    # エイリアス（プレミアム会員用拡張色）
    "niconicowhite":  0xCCCC99,
    "white2":         0xCCCC99,
    "truered":        0xCC0033,
    "red2":           0xCC0033,
    "passionorange":  0xFF6600,
    "orange2":        0xFF6600,
    "madyellow":      0x999900,
    "yellow2":        0x999900,
    "elementalgreen": 0x00CC66,
    "green2":         0x00CC66,
    "marineblue":     0x33FFCC,
    "blue2":          0x33FFCC,
    "nobleviolet":    0x6633CC,
    "purple2":        0x6633CC,
}

# デフォルト（白）の RGB 値
DEFAULT_COLOR_RGB = 0xFFFFFF


# ------------------------------------------------------------------ #
# ユーティリティ関数
# ------------------------------------------------------------------ #

def rgb_to_ass_color(rgb: int) -> str:
    """RGB整数値をASS形式のBGR色文字列に変換する。
    例: 0xFF0000 (赤) → &H000000FF"""
    r = (rgb >> 16) & 0xFF
    g = (rgb >> 8) & 0xFF
    b = rgb & 0xFF
    return f"&H00{b:02X}{g:02X}{r:02X}"


def parse_color(mail_cmd: str) -> int:
    """mail属性からコメント色のRGB値を解析して返す。
    認識できない場合はデフォルト色(白)を返す。"""
    # #RRGGBB 形式
    if mail_cmd.startswith("#") and len(mail_cmd) == 7:
        try:
            return int(mail_cmd[1:], 16)
        except ValueError:
            pass
    # 色名
    return NICOJK_COLORS.get(mail_cmd.lower(), -1)


def seconds_to_ass_time(secs: float) -> str:
    """秒数をASS時刻フォーマット H:MM:SS.CC に変換する。
    CC はセンチ秒（1/100秒）2桁。"""
    secs = max(0.0, secs)
    cs = int(round(secs * 100))  # センチ秒
    h = cs // 360000
    cs %= 360000
    m = cs // 6000
    cs %= 6000
    s = cs // 100
    cs %= 100
    return f"{h}:{m:02d}:{s:02d}.{cs:02d}"


def default_fontsize(height: int) -> int:
    """出力高さに基づいてデフォルトフォントサイズを計算する。"""
    # 720p 基準で線形スケール
    return max(16, int(DEFAULT_FONTSIZE_BASE * height / 720))


# ------------------------------------------------------------------ #
# コメント取得
# ------------------------------------------------------------------ #

def fetch_comments_xml(channel: str, starttime: int, endtime: int,
                       api_base: str) -> str:
    """tsukumijima 過去ログ API からXML文字列を取得する。
    期間が MAX_FETCH_SECONDS を超える場合は複数回に分割してリクエストする。

    Returns:
        XML文字列（全期間のコメントを含む）

    Raises:
        SystemExit: チャンネル不在(100) または一般エラー(1)
    """
    segments = []
    cur = starttime

    while cur < endtime:
        seg_end = min(cur + MAX_FETCH_SECONDS, endtime)
        url = (f"{api_base}/api/kakolog/{channel}"
               f"?starttime={cur}&endtime={seg_end}&format=xml")
        print(f"[nicojk_ass] GET {url}", file=sys.stderr)
        try:
            req = urllib.request.Request(url, headers={
                "User-Agent": "Amatsukaze/nicojk_ass.py",
            })
            with urllib.request.urlopen(req, timeout=60) as resp:
                data = resp.read().decode("utf-8")
        except urllib.error.HTTPError as e:
            # 404 や 400 はチャンネル不在として扱う
            if e.code in (400, 404):
                print(f"[nicojk_ass] チャンネル '{channel}' は存在しません "
                      f"(HTTP {e.code})", file=sys.stderr)
                sys.exit(EXIT_CHANNEL_NOT_FOUND)
            print(f"[nicojk_ass] HTTPエラー: {e}", file=sys.stderr)
            sys.exit(EXIT_ERROR)
        except urllib.error.URLError as e:
            print(f"[nicojk_ass] 接続エラー: {e}", file=sys.stderr)
            sys.exit(EXIT_ERROR)

        segments.append(data)
        cur = seg_end

    return merge_xml_segments(segments)


def merge_xml_segments(segments: list) -> str:
    """複数のXMLセグメント文字列を1つのXML文字列にマージする。
    各セグメントの <packet> 要素内の <chat> 子要素を結合する。"""
    if len(segments) == 1:
        return segments[0]

    # 最初のセグメントをベースにして、残りの <chat> を追加する
    root = ET.fromstring(segments[0])
    for seg in segments[1:]:
        try:
            other = ET.fromstring(seg)
        except ET.ParseError:
            continue
        for chat in other.findall("chat"):
            root.append(chat)

    return ET.tostring(root, encoding="unicode")


# ------------------------------------------------------------------ #
# コメントパース
# ------------------------------------------------------------------ #

class Comment:
    """ニコニコ実況コメントの1件分データ。"""
    __slots__ = ("text", "date", "color_rgb", "size_scale", "position",
                 "start_sec", "end_sec")

    def __init__(self, text: str, date: int, color_rgb: int,
                 size_scale: float, position: str,
                 start_sec: float, end_sec: float):
        self.text = text           # コメント本文
        self.date = date           # 投稿時刻（UNIX秒）
        self.color_rgb = color_rgb # 色（RGB整数）
        self.size_scale = size_scale  # フォントサイズ倍率
        self.position = position   # "naka" / "ue" / "shita"
        self.start_sec = start_sec # ASS開始時刻（秒）
        self.end_sec = end_sec     # ASS終了時刻（秒）


def parse_xml_comments(xml_str: str, starttime: int,
                       duration_sec: float) -> list:
    """XML文字列からコメントリストを構築して返す。

    Args:
        xml_str: tsukumijima APIのXMLレスポンス
        starttime: 番組開始時刻（UNIX秒）。コメント時刻計算の基準
        duration_sec: スクロールコメントの表示秒数

    Returns:
        Comment オブジェクトのリスト（start_sec昇順）
    """
    try:
        root = ET.fromstring(xml_str)
    except ET.ParseError as e:
        print(f"[nicojk_ass] XMLパースエラー: {e}", file=sys.stderr)
        sys.exit(EXIT_ERROR)

    comments = []
    for chat in root.findall("chat"):
        text = chat.text
        if not text:
            continue
        # '/' で始まるコメントはニコニコの制御コメントなのでスキップ
        if text.startswith("/"):
            continue

        # 投稿時刻
        date_str = chat.get("date", "")
        try:
            date = int(date_str)
        except (ValueError, TypeError):
            continue

        # mail 属性から色・サイズ・位置を解析
        mail = chat.get("mail", "") or ""
        color_rgb, size_scale, position = parse_mail(mail)

        # ASS上の時刻を計算（番組先頭からの相対時刻）
        start_sec = float(date - starttime)
        end_sec = start_sec + duration_sec

        comments.append(Comment(
            text=text,
            date=date,
            color_rgb=color_rgb,
            size_scale=size_scale,
            position=position,
            start_sec=start_sec,
            end_sec=end_sec,
        ))

    # 開始時刻でソート
    comments.sort(key=lambda c: c.start_sec)
    return comments


def parse_mail(mail: str):
    """mail属性文字列を解析して (color_rgb, size_scale, position) を返す。

    mail 属性はスペース区切りでコマンドが並ぶ。
    例: "184 red big ue"
    """
    color_rgb = DEFAULT_COLOR_RGB
    size_scale = 1.0
    position = "naka"  # デフォルト: スクロールコメント

    for cmd in mail.split():
        # 色コマンド判定
        c = parse_color(cmd)
        if c != -1:
            color_rgb = c
            continue
        # サイズコマンド判定
        if cmd == "big":
            size_scale = FONTSIZE_SCALE_BIG
        elif cmd == "small":
            size_scale = FONTSIZE_SCALE_SMALL
        elif cmd == "medium":
            size_scale = 1.0
        # 位置コマンド判定
        elif cmd == "ue":
            position = "ue"
        elif cmd == "shita":
            position = "shita"
        elif cmd == "naka":
            position = "naka"
        # 184（匿名）その他は無視

    return color_rgb, size_scale, position


# ------------------------------------------------------------------ #
# 衝突回避アルゴリズム
# ------------------------------------------------------------------ #

class CollisionTracker:
    """コメントの行占有状況を管理し、衝突回避のためのY座標を決定する。

    画面を fontsize 単位の「行」に分割し、各行がいつまで占有されているかを追跡する。
    スクロールコメントと固定コメントで別々にトラッカーを保持する。
    """

    def __init__(self, num_rows: int):
        # 各行の「次に使用可能な時刻（秒）」を保持。初期値は0（空き）
        self.occupy_until = [0.0] * num_rows
        self.num_rows = num_rows

    def find_row(self, start_sec: float, end_sec: float) -> int:
        """指定時刻範囲で空いている行を探して返す。
        全行が埋まっている場合は最も早く空く行を返す（重なりを許容）。"""
        best_row = 0
        best_until = float("inf")
        for row in range(self.num_rows):
            if self.occupy_until[row] <= start_sec:
                # この行は空いているので即確定
                self.occupy_until[row] = end_sec
                return row
            if self.occupy_until[row] < best_until:
                best_until = self.occupy_until[row]
                best_row = row
        # 全行埋まり: 最も早く空く行に重ねる
        self.occupy_until[best_row] = end_sec
        return best_row


# ------------------------------------------------------------------ #
# ASS生成
# ------------------------------------------------------------------ #

def build_ass(comments: list, width: int, height: int,
              font: str, fontsize: int, opacity: float,
              scroll_duration: float) -> str:
    """コメントリストからASS字幕文字列を生成して返す。

    ASS出力フォーマットは C++ の readASS() が期待する形式に合わせる:
      Dialogue: 0,H:MM:SS.CC,H:MM:SS.CC,...
    Layer は必ず 0。
    """
    lines = []

    # [Script Info] セクション
    lines.append("[Script Info]")
    lines.append("ScriptType: v4.00+")
    lines.append(f"PlayResX: {width}")
    lines.append(f"PlayResY: {height}")
    lines.append("Collisions: Normal")
    lines.append("WrapStyle: 2")
    lines.append("ScaledBorderAndShadow: yes")
    lines.append("")

    # [V4+ Styles] セクション
    # makeT() がカンマ区切りトークンを操作するため、フィールド位置は厳守すること
    # Format: Name,Fontname,Fontsize,PrimaryColour,SecondaryColour,
    #         OutlineColour,BackColour,Bold,Italic,Underline,StrikeOut,
    #         ScaleX,ScaleY,Spacing,Angle,BorderStyle,Outline,Shadow,
    #         Alignment,MarginL,MarginR,MarginV,Encoding
    # tokens[0]  = "Style: white" (フィールド0: Name)
    # tokens[3]  = &H00ffffff (PrimaryColour)
    # tokens[4]  = &H00ffffff (SecondaryColour)
    # tokens[5]  = &H00000000 (OutlineColour)
    # tokens[6]  = &H00000000 (BackColour)
    # tokens[16] = Outline幅 → makeT()が '1' に変更
    # tokens[17] = Shadow幅  → makeT()が '0' に変更
    lines.append("[V4+ Styles]")
    lines.append("Format: Name, Fontname, Fontsize, PrimaryColour, "
                 "SecondaryColour, OutlineColour, BackColour, Bold, "
                 "Italic, Underline, StrikeOut, ScaleX, ScaleY, "
                 "Spacing, Angle, BorderStyle, Outline, Shadow, "
                 "Alignment, MarginL, MarginR, MarginV, Encoding")
    # NicoConvAss 互換のスタイル定義（全色分）
    for style_name, rgb in [
        ("white",  0xFFFFFF),
        ("red",    0xFF0000),
        ("pink",   0xFF8080),
        ("orange", 0xFFC000),
        ("yellow", 0xFFFF00),
        ("green",  0x00FF00),
        ("cyan",   0x00FFFF),
        ("blue",   0x0000FF),
        ("purple", 0xC000FF),
        ("black",  0x000000),
    ]:
        color = rgb_to_ass_color(rgb)
        # Bold=-1 (太字), BorderStyle=1 (枠あり), Outline=0, Shadow=4, Alignment=7(左上)
        # makeT()適用後: Outline=1, Shadow=0
        lines.append(
            f"Style: {style_name},{font},{fontsize},"
            f"{color},{color},&H00000000,&H00000000,"
            f"-1,0,0,0,200,200,0,0.00,1,0,4,7,20,20,40,1"
        )
    lines.append("")

    # [Events] セクション
    lines.append("[Events]")
    lines.append("Format: Layer, Start, End, Style, Name, "
                 "MarginL, MarginR, MarginV, Effect, Text")

    # 衝突回避トラッカーの初期化
    # 行数: 画面高さをフォントサイズで割った値
    num_rows = max(1, int(height / fontsize))
    scroll_tracker = CollisionTracker(num_rows)

    # 上固定・下固定コメント用トラッカー（行数を半分に制限）
    fixed_rows = max(1, num_rows // 2)
    top_tracker = CollisionTracker(fixed_rows)
    bottom_tracker = CollisionTracker(fixed_rows)

    # 不透明度からASS透明度値を計算（opacity=1.0→&H00, opacity=0.0→&HFF）
    alpha_val = int((1.0 - min(1.0, max(0.0, opacity))) * 255)

    for cmt in comments:
        # 実際のフォントサイズ（サイズコマンド倍率を適用）
        actual_fs = int(fontsize * cmt.size_scale)

        # ASS色文字列（不透明度を反映）
        r = (cmt.color_rgb >> 16) & 0xFF
        g = (cmt.color_rgb >> 8) & 0xFF
        b = cmt.color_rgb & 0xFF
        ass_color = f"&H{alpha_val:02X}{b:02X}{g:02X}{r:02X}"

        # スタイル名（既存スタイル定義に最近傍の色名を使うか custom を使う）
        style_name = find_nearest_style(cmt.color_rgb)

        # 文字幅の概算（フォントサイズ × 文字数。全角1文字=1フォントサイズ幅と仮定）
        text_width = actual_fs * len(cmt.text)

        start_str = seconds_to_ass_time(cmt.start_sec)
        end_str = seconds_to_ass_time(cmt.end_sec)

        if cmt.position == "naka":
            # スクロールコメント: 右端から左端外へ
            # 衝突回避でY座標（行番号）を決定
            row = scroll_tracker.find_row(cmt.start_sec, cmt.end_sec)
            y = int(fontsize * (row + 0.5))  # 行中央のY座標
            y = min(y, height - actual_fs)   # 画面外にはみ出さない

            # \move(x1,y1,x2,y2): 開始位置(画面右端)→終了位置(左端外)
            x_start = width
            x_end = -text_width
            tag = (f"{{\\move({x_start},{y},{x_end},{y})"
                   f"\\fs{actual_fs}\\c{ass_color}}}")
            text_line = (f"Dialogue: 0,{start_str},{end_str},"
                         f"{style_name},,0000,0000,0000,,{tag}{cmt.text}")

        elif cmt.position == "ue":
            # 上固定コメント: 上部中央に固定表示
            end_str = seconds_to_ass_time(
                cmt.start_sec + FIXED_COMMENT_DURATION)
            row = top_tracker.find_row(cmt.start_sec,
                                       cmt.start_sec + FIXED_COMMENT_DURATION)
            y = int(fontsize * (row + 0.5))
            y = min(y, height // 2 - actual_fs)  # 画面上半分に収める
            y = max(y, actual_fs)

            # \an8: 上中央揃え
            x_center = width // 2
            tag = (f"{{\\an8\\pos({x_center},{y})"
                   f"\\fs{actual_fs}\\c{ass_color}}}")
            text_line = (f"Dialogue: 0,{start_str},{end_str},"
                         f"{style_name},,0000,0000,0000,,{tag}{cmt.text}")

        else:
            # shita: 下固定コメント: 下部中央に固定表示
            end_str = seconds_to_ass_time(
                cmt.start_sec + FIXED_COMMENT_DURATION)
            row = bottom_tracker.find_row(cmt.start_sec,
                                          cmt.start_sec + FIXED_COMMENT_DURATION)
            y = height - int(fontsize * (row + 0.5))
            y = max(y, height // 2 + actual_fs)  # 画面下半分に収める
            y = min(y, height - actual_fs)

            # \an2: 下中央揃え
            x_center = width // 2
            tag = (f"{{\\an2\\pos({x_center},{y})"
                   f"\\fs{actual_fs}\\c{ass_color}}}")
            text_line = (f"Dialogue: 0,{start_str},{end_str},"
                         f"{style_name},,0000,0000,0000,,{tag}{cmt.text}")

        lines.append(text_line)

    lines.append("")
    return "\n".join(lines)


def find_nearest_style(color_rgb: int) -> str:
    """指定したRGB値に最も近いスタイル名を返す。
    スタイル定義済みの色の中から色差（マンハッタン距離）が最小のものを選ぶ。"""
    style_colors = [
        ("white",  0xFFFFFF),
        ("red",    0xFF0000),
        ("pink",   0xFF8080),
        ("orange", 0xFFC000),
        ("yellow", 0xFFFF00),
        ("green",  0x00FF00),
        ("cyan",   0x00FFFF),
        ("blue",   0x0000FF),
        ("purple", 0xC000FF),
        ("black",  0x000000),
    ]
    r0 = (color_rgb >> 16) & 0xFF
    g0 = (color_rgb >> 8) & 0xFF
    b0 = color_rgb & 0xFF
    best_name = "white"
    best_dist = float("inf")
    for name, rgb in style_colors:
        r = (rgb >> 16) & 0xFF
        g = (rgb >> 8) & 0xFF
        b = rgb & 0xFF
        dist = abs(r - r0) + abs(g - g0) + abs(b - b0)
        if dist < best_dist:
            best_dist = dist
            best_name = name
    return best_name


# ------------------------------------------------------------------ #
# エントリポイント
# ------------------------------------------------------------------ #

def parse_args():
    """コマンドライン引数を解析して返す。"""
    parser = argparse.ArgumentParser(
        description="ニコニコ実況 過去ログ取得 & ASS字幕変換スクリプト",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--channel", required=True,
                        help="実況チャンネルID (例: jk1, jk211)")
    parser.add_argument("--starttime", required=True, type=int,
                        help="取得開始時刻（UNIXタイムスタンプ）")
    parser.add_argument("--endtime", required=True, type=int,
                        help="取得終了時刻（UNIXタイムスタンプ）")
    parser.add_argument("--width", required=True, type=int,
                        help="出力解像度 幅 (例: 1920)")
    parser.add_argument("--height", required=True, type=int,
                        help="出力解像度 高さ (例: 1080)")
    parser.add_argument("--output", required=True,
                        help="出力ASSファイルパス")
    parser.add_argument("--font", default=DEFAULT_FONT,
                        help=f"フォント名 (デフォルト: {DEFAULT_FONT!r})")
    parser.add_argument("--fontsize", type=int, default=None,
                        help="フォントサイズ (未指定時は解像度から自動算出)")
    parser.add_argument("--duration", type=float, default=DEFAULT_DURATION,
                        help=f"スクロールコメント表示秒数 (デフォルト: {DEFAULT_DURATION})")
    parser.add_argument("--opacity", type=float, default=DEFAULT_OPACITY,
                        help=f"不透明度 0.0〜1.0 (デフォルト: {DEFAULT_OPACITY})")
    parser.add_argument("--api-url", default=DEFAULT_API_BASE,
                        help=f"過去ログAPIのベースURL (デフォルト: {DEFAULT_API_BASE})")
    return parser.parse_args()


def main():
    args = parse_args()

    # フォントサイズの決定（未指定の場合は解像度から自動算出）
    fontsize = args.fontsize if args.fontsize is not None \
        else default_fontsize(args.height)

    print(f"[nicojk_ass] チャンネル={args.channel} "
          f"開始={args.starttime} 終了={args.endtime} "
          f"解像度={args.width}x{args.height} "
          f"フォント={args.font!r} フォントサイズ={fontsize} "
          f"表示秒数={args.duration} 不透明度={args.opacity}",
          file=sys.stderr)

    # 1. コメントXML取得
    xml_str = fetch_comments_xml(
        args.channel, args.starttime, args.endtime, args.api_url)

    # 2. XMLをパースしてコメントを抽出
    comments = parse_xml_comments(xml_str, args.starttime, args.duration)
    print(f"[nicojk_ass] 取得コメント数: {len(comments)}件", file=sys.stderr)

    # 3. コメントをASS字幕に変換
    ass_content = build_ass(
        comments=comments,
        width=args.width,
        height=args.height,
        font=args.font,
        fontsize=fontsize,
        opacity=args.opacity,
        scroll_duration=args.duration,
    )

    # 4. ASSファイルを出力（UTF-8 BOMなし）
    try:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(ass_content)
    except OSError as e:
        print(f"[nicojk_ass] ファイル書き込みエラー: {e}", file=sys.stderr)
        sys.exit(EXIT_ERROR)

    print(f"[nicojk_ass] ASS出力完了: {args.output}", file=sys.stderr)
    sys.exit(EXIT_SUCCESS)


if __name__ == "__main__":
    main()
