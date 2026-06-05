#!/usr/bin/env python3

# Amatsukaze NicoJK ASS Generator for Linux
# Copyright (c) 2017-2019 Nekopanda
# This software is released under the MIT License.
# http://opensource.org/licenses/mit-license.php

"""
ニコニコ実況 過去ログ取得 & ASS字幕変換スクリプト

tsukumijima 過去ログ API からコメントXMLを取得し、
danmaku2ass でASS字幕に変換する薄いラッパー。

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
import os
import re
import subprocess
import sys
import tempfile
import urllib.request
import urllib.error
import xml.etree.ElementTree as ET

# ------------------------------------------------------------------ #
# 定数定義
# ------------------------------------------------------------------ #

DEFAULT_API_BASE = "https://jikkyo.tsukumijima.net"
MAX_FETCH_SECONDS = 3 * 24 * 3600
DEFAULT_FONT = "MS PGothic"
DEFAULT_FONTSIZE_BASE = 37
DEFAULT_DURATION = 6.0
DEFAULT_OPACITY = 1.0

EXIT_SUCCESS = 0
EXIT_ERROR = 1
EXIT_CHANNEL_NOT_FOUND = 100


# ------------------------------------------------------------------ #
# コメントXML取得
# ------------------------------------------------------------------ #

def fetch_comments_xml(channel, starttime, endtime, api_base):
    """tsukumijima 過去ログ API からXML文字列を取得する。
    期間が MAX_FETCH_SECONDS を超える場合は複数回に分割してリクエストする。"""
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


def merge_xml_segments(segments):
    """複数のXMLセグメントを1つのXML文字列にマージする。"""
    if len(segments) == 1:
        return segments[0]
    root = ET.fromstring(segments[0])
    for seg in segments[1:]:
        try:
            other = ET.fromstring(seg)
        except ET.ParseError:
            continue
        for chat in other.findall("chat"):
            root.append(chat)
    return ET.tostring(root, encoding="unicode")


def rebase_vpos(xml_str, starttime):
    """XMLのvposを番組開始時刻(starttime)基準の相対値に変換する。

    tsukumijima APIのvposは実況スレッド開始（4:00 JST）からのセンチ秒。
    danmaku2assはvposをそのまま出力時刻にするため、相対化が必要。
    date属性（UNIXタイムスタンプ）を使い、starttime基準に変換する。"""
    root = ET.fromstring(xml_str)
    chats = root.findall("chat")
    if not chats:
        return xml_str

    # 最初のコメントの date と vpos からスレッド基準時刻を算出
    first = chats[0]
    first_date = int(first.get("date", "0"))
    first_vpos = int(first.get("vpos", "0"))
    # thread_epoch: vpos=0 に相当するUNIXタイムスタンプ（センチ秒精度）
    thread_epoch_cs = first_date * 100 - first_vpos
    # starttime に対応する vpos オフセット
    base_vpos = starttime * 100 - thread_epoch_cs

    removed = 0
    for chat in list(chats):
        vpos_str = chat.get("vpos")
        if vpos_str is None:
            continue
        new_vpos = int(vpos_str) - base_vpos
        if new_vpos < 0:
            root.remove(chat)
            removed += 1
        else:
            chat.set("vpos", str(new_vpos))

    if removed > 0:
        print(f"[nicojk_ass] vpos相対化: {removed}件の番組開始前コメントを除外",
              file=sys.stderr)

    return ET.tostring(root, encoding="unicode")


# ------------------------------------------------------------------ #
# danmaku2ass 呼び出し
# ------------------------------------------------------------------ #

def find_danmaku2ass():
    """同じディレクトリにある danmaku2ass.py のパスを返す。"""
    script_dir = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(script_dir, "danmaku2ass.py")
    if os.path.isfile(path):
        return path
    print(f"[nicojk_ass] danmaku2ass.py が見つかりません: {path}",
          file=sys.stderr)
    sys.exit(EXIT_ERROR)


def run_danmaku2ass(xml_path, output_path, width, height,
                    font, fontsize, opacity, duration):
    """danmaku2ass.py をサブプロセスで実行する。"""
    danmaku2ass = find_danmaku2ass()
    cmd = [
        sys.executable, danmaku2ass,
        "-f", "Niconico",
        "-s", f"{width}x{height}",
        "-fn", font,
        "-fs", str(fontsize),
        "-a", str(opacity),
        "-dm", str(duration),
        "-ds", str(duration),
        "-o", output_path,
        xml_path,
    ]
    print(f"[nicojk_ass] danmaku2ass: {' '.join(cmd)}", file=sys.stderr)
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.stdout:
        print(result.stdout, end="", file=sys.stderr)
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    if result.returncode != 0:
        print(f"[nicojk_ass] danmaku2ass が失敗しました (exit={result.returncode})",
              file=sys.stderr)
        sys.exit(EXIT_ERROR)


# ------------------------------------------------------------------ #
# ASS後処理（readASS() 互換化）
# ------------------------------------------------------------------ #

# NicoConvAssスタイル色定義（ASS形式: &HAABBGGRR）
_NICOCONVASS_COLORS = [
    ("white",  "&H00ffffff"),
    ("red",    "&H000000ff"),
    ("pink",   "&H009314ff"),
    ("orange", "&H00008cff"),
    ("yellow", "&H0000ffff"),
    ("green",  "&H00008000"),
    ("cyan",   "&H00ffff00"),
    ("blue",   "&H00ff0000"),
    ("purple", "&H00800080"),
    ("black",  "&H00000000"),
]

_MOVE_RE = re.compile(
    r"\\move\(\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\)")
_POS_RE = re.compile(r"\\pos\(\s*(-?\d+)\s*,\s*(-?\d+)\s*\)")
_COLOR_TAG_RE = re.compile(r"\\c&H([0-9A-Fa-f]{6})&")
_3COLOR_TAG_RE = re.compile(r"\\3c&H[0-9A-Fa-f]+&")


def _convert_color_bt709(rgb):
    """BT.601→BT.709色空間変換（danmaku2assのConvertColor相当）。BGR hex文字列を返す。"""
    if rgb == 0x000000:
        return "000000"
    if rgb == 0xffffff:
        return "FFFFFF"
    R = (rgb >> 16) & 0xff
    G = (rgb >> 8) & 0xff
    B = rgb & 0xff
    clip = lambda x: max(0, min(255, round(x)))
    return "%02X%02X%02X" % (
        clip(R * 0.00956384088080656 + G * 0.03217254540203729
             + B * 0.95826361371715607),
        clip(R * -0.10493933142075390 + G * 1.17231478191855154
             + B * -0.06737545049779757),
        clip(R * 0.91348912373987645 + G * 0.07858536372532510
             + B * 0.00792551253479842))


def _build_bgr_to_style_map():
    """danmaku2assのBT.709変換後BGR値→NicoConvAssスタイル名の逆引きテーブル"""
    nico_color_to_style = {
        0xff0000: "red", 0xff8080: "pink", 0xffcc00: "orange",
        0xffff00: "yellow", 0x00ff00: "green", 0x00ffff: "cyan",
        0x0000ff: "blue", 0xc000ff: "purple", 0x000000: "black",
        # 拡張色（niconicowhite, truered, passionorange 等）
        0xcccc99: "white", 0xcc0033: "red", 0xff6600: "orange",
        0x999900: "yellow", 0x00cc66: "green", 0x33ffcc: "cyan",
        0x6633cc: "purple",
    }
    result = {}
    for rgb, style in nico_color_to_style.items():
        bgr = _convert_color_bt709(rgb)
        result[bgr.upper()] = style
    return result


_BGR_TO_STYLE = _build_bgr_to_style_map()


def _fixup_dialogue(line, y_offset):
    """Dialogue行をNicoConvAss互換形式に変換する。"""
    if not line.startswith("Dialogue:"):
        return line
    after = line[len("Dialogue:"):].lstrip()
    parts = after.split(",", 9)
    if len(parts) < 10:
        return line

    text = parts[9]

    # 色タグからスタイル名を決定
    style_name = "white"
    cm = _COLOR_TAG_RE.search(text)
    if cm:
        bgr = cm.group(1).upper()
        style_name = _BGR_TO_STYLE.get(bgr, "white")

    # 色タグ除去（Style側で色指定）
    text = _COLOR_TAG_RE.sub("", text)
    text = _3COLOR_TAG_RE.sub("", text)

    # moveのY座標にオフセット追加（上端見切れ防止）
    text = _MOVE_RE.sub(
        lambda m: "\\move(%s,%d,%s,%d)" % (
            m.group(1), int(m.group(2)) + y_offset,
            m.group(3), int(m.group(4)) + y_offset),
        text)

    # posのY座標にもオフセット追加（ただし下揃え\an2は除く）
    if "\\an2" not in text:
        text = _POS_RE.sub(
            lambda m: "\\pos(%s,%d)" % (
                m.group(1), int(m.group(2)) + y_offset),
            text)

    parts[0] = "0"
    parts[3] = style_name
    parts[9] = text
    return "Dialogue: " + ",".join(parts)


def fixup_ass(ass_path, width, height, fontsize, font):
    """danmaku2ass出力をNicoConvAss互換形式に変換する。"""
    with open(ass_path, "r", encoding="utf-8-sig") as f:
        content = f.read()

    dialogues = []
    for line in content.splitlines():
        if line.startswith("Dialogue:"):
            dialogues.append(line)

    y_offset = max(1, round(height / 120))
    out = []

    # [Script Info]
    out.append("[Script Info]")
    out.append("ScriptType: v4.00+")
    out.append("Collisions: Normal")
    out.append("ScaledBorderAndShadow: Yes")
    out.append("PlayResX: %d" % width)
    out.append("PlayResY: %d" % height)
    out.append("Timer: 100.0000")
    out.append("WrapStyle: 0")
    out.append("")

    # [V4+ Styles]
    out.append("[V4+ Styles]")
    out.append("Format: Name, Fontname, Fontsize, PrimaryColour, "
               "SecondaryColour, OutlineColour, BackColour, Bold, Italic, "
               "Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, "
               "BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, "
               "MarginV, Encoding")
    for name, color in _NICOCONVASS_COLORS:
        out.append(
            "Style: %s,%s,%d,%s,%s,&H00000000,&H00000000,"
            "-1,0,0,0,100,100,0,0.00,1,0,4,7,20,20,40,1"
            % (name, font, fontsize, color, color))
    out.append("")

    # [Events]
    out.append("[Events]")
    out.append("Format: Layer, Start, End, Style, Name, MarginL, "
               "MarginR, MarginV, Effect, Text")
    for d in dialogues:
        out.append(_fixup_dialogue(d, y_offset))

    with open(ass_path, "w", encoding="utf-8", newline="") as f:
        f.write("\r\n".join(out))
        if out:
            f.write("\r\n")


# ------------------------------------------------------------------ #
# エントリポイント
# ------------------------------------------------------------------ #

def default_fontsize(height):
    return max(16, int(DEFAULT_FONTSIZE_BASE * height / 720))


def parse_args():
    parser = argparse.ArgumentParser(
        description="ニコニコ実況 過去ログ取得 & ASS字幕変換スクリプト",
    )
    parser.add_argument("--channel", required=True,
                        help="実況チャンネルID (例: jk1, jk211)")
    parser.add_argument("--starttime", required=True, type=int,
                        help="取得開始時刻（UNIXタイムスタンプ）")
    parser.add_argument("--endtime", required=True, type=int,
                        help="取得終了時刻（UNIXタイムスタンプ）")
    parser.add_argument("--width", required=True, type=int,
                        help="出力解像度 幅")
    parser.add_argument("--height", required=True, type=int,
                        help="出力解像度 高さ")
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

    # コメント数を表示
    try:
        root = ET.fromstring(xml_str)
        n_comments = len(root.findall("chat"))
        print(f"[nicojk_ass] 取得コメント数: {n_comments}件", file=sys.stderr)
    except ET.ParseError:
        pass

    # 2. vpos を番組開始時刻基準に相対化
    xml_str = rebase_vpos(xml_str, args.starttime)

    # 3. 一時XMLファイルに保存して danmaku2ass に渡す
    with tempfile.NamedTemporaryFile(
            mode="w", suffix=".xml", encoding="utf-8", delete=False) as tmp:
        tmp.write(xml_str)
        tmp_xml = tmp.name

    try:
        run_danmaku2ass(
            tmp_xml, args.output,
            args.width, args.height,
            args.font, fontsize, args.opacity, args.duration,
        )
    finally:
        os.unlink(tmp_xml)

    # 4. NicoConvAss互換形式への変換（Style/Layer/座標/色タグ等）
    fixup_ass(args.output, args.width, args.height, fontsize, args.font)

    print(f"[nicojk_ass] ASS出力完了: {args.output}", file=sys.stderr)
    sys.exit(EXIT_SUCCESS)


if __name__ == "__main__":
    main()
