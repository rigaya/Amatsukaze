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
DEFAULT_FONTSIZE_BASE = 36
DEFAULT_DURATION = 5.0
DEFAULT_OPACITY = 0.8

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

_DIALOGUE_RE = re.compile(r"^Dialogue:\s*2,", re.MULTILINE)

def fixup_ass_layer(ass_path):
    """danmaku2ass が出力する Dialogue: 2, を readASS() 互換の Dialogue: 0, に補正する。"""
    with open(ass_path, "r", encoding="utf-8-sig") as f:
        content = f.read()
    content = _DIALOGUE_RE.sub("Dialogue: 0,", content)
    with open(ass_path, "w", encoding="utf-8") as f:
        f.write(content)


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

    # 2. 一時XMLファイルに保存して danmaku2ass に渡す
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

    # 3. Layer 補正（danmaku2ass は Dialogue: 2 で出力するが readASS() は 0 を期待）
    fixup_ass_layer(args.output)

    print(f"[nicojk_ass] ASS出力完了: {args.output}", file=sys.stderr)
    sys.exit(EXIT_SUCCESS)


if __name__ == "__main__":
    main()
