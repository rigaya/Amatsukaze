#!/usr/bin/env python3
"""戻り値を使っていない前置 ++ / -- を列挙する。

対象はリポジトリ内の自前ソース (C / C++ / C# / Razor)。
libfaad, TVCaptionMod2, NicoJK18Client, FileCutter など
同梱の別プロジェクトと、obj などの生成物は見ない。

「戻り値を使っていない」とは、後置に変えても式の値を読む箇所がないこと。

- 式文: ++x;  --x;  ++a, ++b;
- if / while / for の本体文: if (c) ++x;
- for の初期化式・増分式で、カンマ区切りの項全体が前置 ++/-- であるもの
  例: for (...; ...; ++i) / for (...; ...; ++i, --j)

次は対象外 (値を読んでいる):

- return ++x;  a = ++x;  foo(++x);  a[++x];  if (++x)
- for の条件式 for (; ++x < n; )
- 項の一部 for (...; ...; foo(++x))

Razor は @code / @functions / @{ } / @( )、制御構文のヘッダ、
制御構文の本体のうち HTML タグの外に出ている C# を見る。
<style> やタグ内の CSS (var(--name) など) は見ない。

クラス型 (イテレータ等) では、値が捨てられていても後置はコピーが
増えることがある。このスクリプトは値の使用有無だけを見る。

終了コードは、該当があれば 1、なければ 0。
"""

from __future__ import annotations

import argparse
import os
import sys
from dataclasses import dataclass


EXTS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".cs", ".razor"}

# 走査中に中へ入らないディレクトリ名。
SKIP_DIR_NAMES = {
    ".git", ".vs", ".vscode", ".claude", ".codex", ".github",
    "bin", "obj", "node_modules", "packages",
    "x64", "x86", "Debug", "Release", "ARM64",
}

# リポジトリ直下の、自前ソースではないもの。
SKIP_TOP = {
    "libfaad", "TVCaptionMod2", "TVCaption2Mod", "NicoJK18Client", "FileCutter",
    "googletest", "ffmpeg_lgpl", "include", "include_gpl", "include_win",
    "lib", "artifacts", "publish", "build", "data", "defaults", "doc", "docker",
}

# 直前にあると、続く文の値は捨てられる。
_VOID_KEYWORDS = {"else", "do"}
# 直前にあると、式の値は読まれる (return ++x など)。
_VALUE_KEYWORDS = {"return", "throw", "co_return", "yield", "await", "sizeof", "typeof", "new", "delete"}
# ) の直前なら、その ) の後の文の値は捨てられる。
_CONTROL_KEYWORDS = {"if", "while", "for", "foreach", "switch", "lock", "using", "fixed", "catch"}

_VOID_HTML = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"}
_RAZOR_CODE_BLOCKS = {"code", "functions"}
_RAZOR_CONTROLS = {"for", "foreach", "if", "while", "switch", "lock", "using", "catch"}


@dataclass(frozen=True)
class Hit:
    index: int
    kind: str
    text: str


def _is_ident_char(ch: str, start: bool = False) -> bool:
    if start:
        return ch.isalpha() or ch == "_"
    return ch.isalnum() or ch == "_"


def _skip_ws(text: str, i: int) -> int:
    n = len(text)
    while i < n and text[i] in " \t\r\n":
        i += 1
    return i


def _skip_ws_back(text: str, i: int) -> int:
    """i の直前から非空白の位置を返す。無ければ -1。"""
    j = i - 1
    while j >= 0 and text[j] in " \t\r\n":
        j -= 1
    return j


def _keyword_ending_at(text: str, last: int) -> str:
    if last < 0 or not _is_ident_char(text[last]):
        return ""
    end = last + 1
    j = last
    while j >= 0 and _is_ident_char(text[j]):
        j -= 1
    if j >= 0 and _is_ident_char(text[j]):
        return ""
    return text[j + 1:end]


def _string_prefix(text: str, quote: int) -> str:
    j = quote - 1
    while j >= 0 and (text[j].isalnum() or text[j] in "@$"):
        j -= 1
    return text[j + 1:quote]


def _consume_c_string(text: str, quote: int) -> int | None:
    """quote 位置の " から始まる通常文字列の終端 (終了引用符の次)。"""
    i = quote + 1
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == "\\":
            i += 2
            continue
        if ch == "\n":
            return None
        if ch == '"':
            return i + 1
        i += 1
    return None


def _consume_cs_string(text: str, quote: int, verbatim: bool, interpolated: bool) -> int | None:
    """C# の " 文字列。補間穴の中の文字列も終端判定に使う。"""
    i = quote + 1
    n = len(text)
    depth = 0
    while i < n:
        ch = text[i]
        if depth > 0:
            skipped = _try_consume_noncode(text, i, "cs")
            if skipped is not None and skipped != i:
                # 穴の中の文字列・コメント。自分自身の開始引用符は対象外。
                if not (skipped > quote and text[i] == '"' and i == quote):
                    if text[i] != '"' or _string_prefix(text, i) or verbatim or interpolated:
                        # 穴の先頭で同じ文字列を再入しない。
                        if i != quote:
                            i = skipped
                            continue
        if not verbatim and ch == "\\":
            i += 2
            continue
        if ch == '"':
            if verbatim and i + 1 < n and text[i + 1] == '"':
                i += 2
                continue
            if depth == 0:
                return i + 1
        if interpolated and ch == "{":
            if i + 1 < n and text[i + 1] == "{" and depth == 0:
                i += 2
                continue
            depth += 1
            i += 1
            continue
        if interpolated and ch == "}" and depth > 0:
            if i + 1 < n and text[i + 1] == "}" and depth == 1:
                # 穴の中の }} はエスケープではない。深さ 0 の }} だけがリテラル。
                pass
            depth -= 1
            i += 1
            continue
        if interpolated and depth == 0 and ch == "}" and i + 1 < n and text[i + 1] == "}":
            i += 2
            continue
        if not verbatim and ch == "\n" and depth == 0:
            return None
        i += 1
    return None


def _consume_raw_string(text: str, quote: int) -> int | None:
    """C++ の R\"delim(...)delim\" 。quote は R の直後の \"。"""
    n = len(text)
    j = quote + 1
    while j < n and text[j] != "(":
        if text[j] in " \t\r\n)":
            return None
        j += 1
    if j >= n:
        return None
    delim = text[quote + 1:j]
    end_mark = ")" + delim + '"'
    k = text.find(end_mark, j + 1)
    if k < 0:
        return None
    return k + len(end_mark)


def _consume_char(text: str, quote: int) -> int | None:
    n = len(text)
    # 1'000 のような桁区切りは文字リテラルではない。
    if quote > 0 and quote + 1 < n and text[quote - 1].isdigit() and text[quote + 1].isdigit():
        return None
    i = quote + 1
    while i < n:
        ch = text[i]
        if ch == "\\":
            i += 2
            continue
        if ch == "'":
            return i + 1
        if ch == "\n":
            return None
        i += 1
    return None


def _consume_line_comment(text: str, i: int) -> int:
    j = text.find("\n", i + 2)
    return len(text) if j < 0 else j


def _consume_block_comment(text: str, i: int) -> int:
    j = text.find("*/", i + 2)
    return len(text) if j < 0 else j + 2


def _try_consume_noncode(text: str, i: int, lang: str) -> int | None:
    """コメントか文字列なら、その直後の位置。そうでなければ None。"""
    n = len(text)
    if i >= n:
        return None
    if text.startswith("//", i):
        return _consume_line_comment(text, i)
    if text.startswith("/*", i):
        return _consume_block_comment(text, i)
    ch = text[i]
    if ch == '"':
        prefix = _string_prefix(text, i)
        if lang == "cpp" and prefix.endswith("R") and prefix[:-1] in {"", "u", "U", "L", "u8"}:
            return _consume_raw_string(text, i)
        if lang == "cs" and prefix in {"", "$", "@", "$@", "@$"}:
            return _consume_cs_string(text, i, verbatim="@" in prefix, interpolated="$" in prefix)
        if lang == "cpp" and prefix in {"", "u", "U", "L", "u8"}:
            return _consume_c_string(text, i)
        # 識別子の直後の " も文字列として扱う (プレフィックスなし)。
        if prefix == "" or not any(_is_ident_char(c) for c in prefix):
            return _consume_c_string(text, i) if lang == "cpp" else _consume_cs_string(text, i, False, False)
        if lang == "cs":
            return _consume_cs_string(text, i, False, False)
        return _consume_c_string(text, i)
    if ch == "'":
        return _consume_char(text, i)
    return None


def _match_closer(text: str, open_index: int, open_ch: str, close_ch: str, lang: str) -> int:
    """open_index の開き括弧に対応する閉じ括弧。文字列とコメントは飛ばす。"""
    n = len(text)
    depth = 1
    i = open_index + 1
    while i < n:
        skipped = _try_consume_noncode(text, i, lang)
        if skipped is not None:
            i = skipped
            continue
        ch = text[i]
        if ch == open_ch:
            depth += 1
        elif ch == close_ch:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def mask_comments_and_strings(text: str, lang: str) -> str:
    """コメントと文字列を空白にする。改行と文字位置は保つ。"""
    chars = list(text)
    n = len(chars)
    i = 0

    def blank(a: int, b: int) -> None:
        for k in range(a, b):
            if chars[k] != "\n":
                chars[k] = " "

    while i < n:
        skipped = _try_consume_noncode(text, i, lang)
        if skipped is not None and skipped > i:
            blank(i, skipped)
            i = skipped
            continue
        i += 1
    return "".join(chars)


def _is_prefix_op(text: str, i: int) -> bool:
    """前置なら True。識別子の直後 (i++ や operator++) は後置。

    else / do の直後、if (c) の ')' の直後は前置の文になり得る。
    ')' や ']' の直後でも、左辺値が続けば前置として候補に残し、
    値を捨てる文かどうかは呼び出し側で判断する。
    """
    if not text.startswith(("++", "--"), i):
        return False
    j = _skip_ws_back(text, i)
    if j < 0:
        return True
    if _is_ident_char(text[j]):
        return _keyword_ending_at(text, j) in _VOID_KEYWORDS
    return True


def _match_bracket(text: str, open_index: int) -> int:
    depth = 1
    i = open_index + 1
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
            if depth == 0:
                return i
        elif ch == "(":
            end = _match_bracket_paren(text, i)
            if end < 0:
                return -1
            i = end + 1
            continue
        i += 1
    return -1


def _match_bracket_paren(text: str, open_index: int) -> int:
    depth = 1
    i = open_index + 1
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def _parse_lvalue(text: str, i: int) -> int | None:
    """前置 ++/-- のオペランド終端。i は演算子の直後。"""
    n = len(text)
    i = _skip_ws(text, i)
    while i < n and text[i] == "*":
        i += 1
        i = _skip_ws(text, i)
    if i >= n:
        return None
    if text[i] == "(":
        end = _match_bracket_paren(text, i)
        return None if end < 0 else end + 1
    if not _is_ident_char(text[i], start=True):
        return None
    i += 1
    while i < n and _is_ident_char(text[i]):
        i += 1
    while True:
        j = _skip_ws(text, i)
        if text.startswith("->", j) or text.startswith("::", j):
            j = _skip_ws(text, j + 2)
            if j >= n or not _is_ident_char(text[j], start=True):
                break
            j += 1
            while j < n and _is_ident_char(text[j]):
                j += 1
            i = j
            continue
        if j < n and text[j] == ".":
            j = _skip_ws(text, j + 1)
            if j >= n or not _is_ident_char(text[j], start=True):
                break
            j += 1
            while j < n and _is_ident_char(text[j]):
                j += 1
            i = j
            continue
        if j < n and text[j] == "[":
            end = _match_bracket(text, j)
            if end < 0:
                return None
            i = end + 1
            continue
        if j < n and text[j] == "(":
            # 参照を返す呼び出し。++get();
            end = _match_bracket_paren(text, j)
            if end < 0:
                return None
            i = end + 1
            continue
        break
    return i


def _is_ternary_colon(text: str, colon: int) -> bool:
    """この ':' が三項演算子なら True。case / ラベルの ':' は False。"""
    dp = db = dc = 0
    i = colon - 1
    while i >= 0:
        ch = text[i]
        if ch in " \t\r\n":
            i -= 1
            continue
        if ch == ")":
            dp += 1
        elif ch == "(":
            if dp == 0:
                return False
            dp -= 1
        elif ch == "]":
            db += 1
        elif ch == "[":
            if db == 0:
                return False
            db -= 1
        elif ch == "}":
            dc += 1
        elif ch == "{":
            if dc == 0:
                return False
            dc -= 1
        elif dp == db == dc == 0 and ch == "?":
            return True
        elif dp == db == dc == 0 and ch in ";{}":
            return False
        i -= 1
    return False


def _is_void_before(text: str, op_index: int) -> bool:
    j = _skip_ws_back(text, op_index)
    if j < 0:
        return True
    ch = text[j]
    if ch in "{};":
        return True
    if ch == ")":
        open_i = _match_back_paren(text, j)
        if open_i < 0:
            return False
        return _keyword_ending_at(text, _skip_ws_back(text, open_i)) in _CONTROL_KEYWORDS
    if ch == ":":
        return not _is_ternary_colon(text, j)
    return _keyword_ending_at(text, j) in _VOID_KEYWORDS


def _match_back_paren(text: str, close_index: int) -> int:
    depth = 0
    i = close_index
    while i >= 0:
        ch = text[i]
        if ch == ")":
            depth += 1
        elif ch == "(":
            depth -= 1
            if depth == 0:
                return i
        i -= 1
    return -1


def _reaches_semicolon(text: str, i: int) -> bool:
    """i (オペランドの直後) から、深さ 0 の ';' に ')' や '}' より先に着くか。"""
    n = len(text)
    dp = db = dc = 0
    while i < n:
        ch = text[i]
        if ch in " \t\r\n":
            i += 1
            continue
        if ch == "(":
            dp += 1
        elif ch == ")":
            if dp == 0:
                return False
            dp -= 1
        elif ch == "[":
            db += 1
        elif ch == "]":
            if db == 0:
                return False
            db -= 1
        elif ch == "{":
            dc += 1
        elif ch == "}":
            if dc == 0:
                return False
            dc -= 1
        elif ch == ";" and dp == db == dc == 0:
            return True
        i += 1
    return False


def _comma_is_statement_level(text: str, comma_index: int) -> bool:
    """カンマが式文のカンマ演算子なら True。引数リストや return の式なら False。"""
    dp = db = dc = 0
    i = comma_index - 1
    while i >= 0:
        ch = text[i]
        if ch in " \t\r\n":
            i -= 1
            continue
        if ch == ")":
            dp += 1
        elif ch == "(":
            if dp == 0:
                return False
            dp -= 1
        elif ch == "]":
            db += 1
        elif ch == "[":
            if db == 0:
                return False
            db -= 1
        elif ch == "}":
            dc += 1
        elif ch == "{":
            if dc == 0:
                return True
            dc -= 1
        elif dp == db == dc == 0 and ch == ";":
            return True
        elif dp == db == dc == 0 and _keyword_ending_at(text, i) in _VALUE_KEYWORDS:
            return False
        i -= 1
    return True


def _split_top_level(text: str, sep: str) -> list[tuple[int, int]]:
    """深さ 0 の sep で区切った [start, end) の列。マスク済みテキスト用。"""
    parts: list[tuple[int, int]] = []
    start = 0
    dp = db = dc = 0
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == "(":
            dp += 1
        elif ch == ")":
            dp = max(0, dp - 1)
        elif ch == "[":
            db += 1
        elif ch == "]":
            db = max(0, db - 1)
        elif ch == "{":
            dc += 1
        elif ch == "}":
            dc = max(0, dc - 1)
        if ch == sep and dp == db == dc == 0:
            parts.append((start, i))
            start = i + 1
        i += 1
    parts.append((start, n))
    return parts


def _exact_preinc(text: str, start: int, end: int) -> int | None:
    """[start, end) 全体が前置 ++/-- の左辺値なら、演算子の位置を返す。"""
    i = start
    n = end
    while i < n and text[i] in " \t\r\n":
        i += 1
    if i >= n or not text.startswith(("++", "--"), i):
        return None
    lvalue_end = _parse_lvalue(text, i + 2)
    if lvalue_end is None or lvalue_end > n:
        return None
    j = lvalue_end
    while j < n and text[j] in " \t\r\n":
        j += 1
    if j != n:
        return None
    return i


def _find_for_hits(text: str) -> list[Hit]:
    hits: list[Hit] = []
    n = len(text)
    i = 0
    while i < n:
        if text.startswith("for", i) and _is_ident_boundary(text, i, 3):
            j = _skip_ws(text, i + 3)
            if j < n and text[j] == "(":
                end = _match_bracket_paren(text, j)
                if end >= 0:
                    header = text[j + 1:end]
                    parts = _split_top_level(header, ";")
                    if len(parts) == 3:
                        base = j + 1
                        for clause, kind in ((0, "for初期化"), (2, "for増分")):
                            a, b = parts[clause]
                            for cs, ce in _split_top_level(header[a:b], ","):
                                op = _exact_preinc(text, base + a + cs, base + a + ce)
                                if op is not None:
                                    lvalue_end = _parse_lvalue(text, op + 2)
                                    snippet = text[op:lvalue_end if lvalue_end is not None else op + 2]
                                    hits.append(Hit(op, kind, " ".join(snippet.split())))
                    i = end + 1
                    continue
        i += 1
    return hits


def _is_ident_boundary(text: str, i: int, length: int) -> bool:
    if i > 0 and _is_ident_char(text[i - 1]):
        return False
    end = i + length
    if end < len(text) and _is_ident_char(text[end]):
        return False
    return True


def _find_statement_hits(text: str) -> list[Hit]:
    hits: list[Hit] = []
    n = len(text)
    i = 0
    while i < n:
        if _is_prefix_op(text, i):
            lvalue_end = _parse_lvalue(text, i + 2)
            if lvalue_end is not None:
                k = _skip_ws(text, lvalue_end)
                if k < n and text[k] in ",;" and _reaches_semicolon(text, k):
                    discarded = False
                    if _is_void_before(text, i):
                        discarded = True
                    else:
                        comma = _skip_ws_back(text, i)
                        if comma >= 0 and text[comma] == "," and _comma_is_statement_level(text, comma):
                            discarded = True
                    if discarded:
                        hits.append(Hit(i, "文", " ".join(text[i:lvalue_end].split())))
                        i = lvalue_end
                        continue
        i += 1
    return hits


def scan_masked(text: str) -> list[Hit]:
    hits = _find_statement_hits(text) + _find_for_hits(text)
    hits.sort(key=lambda h: h.index)
    # for 増分と式文の両方が同じ位置を拾った場合は for 側を残す。
    unique: list[Hit] = []
    seen: set[int] = set()
    for hit in sorted(hits, key=lambda h: (h.index, h.kind != "文")):
        if hit.index in seen:
            continue
        seen.add(hit.index)
        unique.append(hit)
    unique.sort(key=lambda h: h.index)
    return unique


def _is_html_tag_start(text: str, i: int) -> bool:
    n = len(text)
    if i >= n or text[i] != "<":
        return False
    nxt = text[i + 1] if i + 1 < n else ""
    if not (nxt.isalpha() or nxt in "/!"):
        return False
    j = _skip_ws_back(text, i)
    if j >= 0 and (_is_ident_char(text[j]) or text[j] in ")]"):
        return False
    return True


def _skip_quoted_html(text: str, i: int, quote: str) -> int:
    i += 1
    n = len(text)
    while i < n:
        if text[i] == "\\":
            i += 2
            continue
        if text[i] == quote:
            return i + 1
        i += 1
    return i


def _skip_html_tag(text: str, i: int) -> tuple[int, str, bool]:
    """タグ開始 '<' から、タグ終了の次・タグ名・要素全体を飛ばすか。"""
    n = len(text)
    if text.startswith("<!--", i):
        k = text.find("-->", i + 4)
        return (n if k < 0 else k + 3, "!", True)
    j = i + 1
    closing = False
    if j < n and text[j] == "/":
        closing = True
        j += 1
    if j < n and text[j] == "!":
        k = text.find(">", j)
        return (n if k < 0 else k + 1, "!", True)
    name_start = j
    while j < n and (text[j].isalnum() or text[j] in "_-:"):
        j += 1
    name = text[name_start:j].lower()
    self_closing = closing
    while j < n:
        ch = text[j]
        if ch in "\"'":
            j = _skip_quoted_html(text, j, ch)
            continue
        if ch == "/":
            self_closing = True
        if ch == ">":
            whole = self_closing or closing or name in _VOID_HTML
            return j + 1, name, whole
        j += 1
    return n, name, True


def _skip_html_element(text: str, i: int) -> int:
    """<style> / <script> の要素全体の次。"""
    after, name, whole = _skip_html_tag(text, i)
    if whole:
        return after
    close = f"</{name}"
    n = len(text)
    k = after
    lower = text.lower()
    while k < n:
        found = lower.find(close, k)
        if found < 0:
            return n
        after_close, _, _ = _skip_html_tag(text, found)
        return after_close
    return n


def _razor_keyword(text: str, i: int) -> tuple[str, int] | None:
    """i は '@' の次。キーワードと直後の位置。"""
    n = len(text)
    if i >= n or not _is_ident_char(text[i], start=True):
        return None
    j = i + 1
    while j < n and _is_ident_char(text[j]):
        j += 1
    return text[i:j], j


def blank_razor_non_code(text: str) -> str:
    """Razor のうち C# 以外を空白にする。文字位置と改行は保つ。"""
    n = len(text)
    keep = [ch == "\n" for ch in text]
    lang = "cs"

    def keep_range(a: int, b: int) -> None:
        for k in range(a, min(b, n)):
            keep[k] = True

    def walk_markup(begin: int, end: int) -> int:
        i = begin
        while i < end:
            if text.startswith("<style", i) and _is_html_tag_start(text, i):
                i = _skip_html_element(text, i)
                continue
            if text.startswith("<script", i) and _is_html_tag_start(text, i):
                i = _skip_html_element(text, i)
                continue
            if text.startswith("<!--", i):
                k = text.find("-->", i + 4)
                i = end if k < 0 else k + 3
                continue
            if text.startswith("@*", i):
                k = text.find("*@", i + 2)
                i = end if k < 0 else k + 2
                continue
            if text.startswith("@@", i):
                i += 2
                continue
            if text[i] == "@":
                i = handle_at(i, end)
                continue
            i += 1
        return i

    def handle_at(at: int, limit: int) -> int:
        nxt = at + 1
        if nxt >= limit:
            return limit
        if text[nxt] == "{":
            end = _match_closer(text, nxt, "{", "}", lang)
            if end < 0:
                return limit
            keep_range(nxt, end + 1)
            return end + 1
        if text[nxt] == "(":
            end = _match_closer(text, nxt, "(", ")", lang)
            if end < 0:
                return limit
            keep_range(nxt, end + 1)
            return end + 1
        kw = _razor_keyword(text, nxt)
        if kw is None:
            return nxt
        name, after = kw
        k = _skip_ws(text, after)
        if name in _RAZOR_CODE_BLOCKS and k < limit and text[k] == "{":
            end = _match_closer(text, k, "{", "}", lang)
            if end < 0:
                return limit
            keep_range(k, end + 1)
            return end + 1
        if name in _RAZOR_CONTROLS and k < limit and text[k] == "(":
            end = _match_closer(text, k, "(", ")", lang)
            if end < 0:
                return limit
            # for パーサがキーワードを見られるよう、キーワードから括弧まで残す。
            keep_range(nxt, end + 1)
            body = _skip_ws(text, end + 1)
            if body < limit and text[body] == "{":
                return walk_mixed(body, limit)
            return end + 1
        # @using などの行ディレクティブ。
        nl = text.find("\n", at)
        return limit if nl < 0 else nl + 1

    def walk_mixed(open_brace: int, limit: int) -> int:
        """制御構文の { } 。タグの外は C# として残す。"""
        keep[open_brace] = True
        i = open_brace + 1
        depth = 1
        while i < limit and depth:
            if text.startswith("@*", i):
                k = text.find("*@", i + 2)
                i = limit if k < 0 else k + 2
                continue
            if text.startswith("@@", i):
                i += 2
                continue
            if text[i] == "@":
                i = handle_at(i, limit)
                continue
            if text.startswith("<style", i) and _is_html_tag_start(text, i):
                i = _skip_html_element(text, i)
                continue
            if text.startswith("<script", i) and _is_html_tag_start(text, i):
                i = _skip_html_element(text, i)
                continue
            if _is_html_tag_start(text, i):
                after, name, whole = _skip_html_tag(text, i)
                if whole or name in _VOID_HTML:
                    i = after
                    continue
                # 開始タグだけ飛ばし、中身は引き続き見る。
                i = after
                continue
            if text[i] == "{":
                depth += 1
                keep[i] = True
                i += 1
                continue
            if text[i] == "}":
                depth -= 1
                keep[i] = True
                i += 1
                continue
            keep[i] = True
            i += 1
        return i

    walk_markup(0, n)
    out = []
    for idx, ch in enumerate(text):
        if keep[idx] or ch == "\n":
            out.append(ch)
        else:
            out.append(" ")
    return "".join(out)


def prepare(text: str, lang: str) -> str:
    if lang == "razor":
        text = blank_razor_non_code(text)
        lang = "cs"
    return mask_comments_and_strings(text, lang)


def line_col(text: str, index: int) -> tuple[int, int]:
    line = text.count("\n", 0, index) + 1
    line_start = text.rfind("\n", 0, index) + 1
    col = index - line_start + 1 - text.count("\r", line_start, index)
    return line, col


def scan_text(text: str, lang: str) -> list[tuple[int, int, Hit]]:
    masked = prepare(text, lang)
    found = []
    for hit in scan_masked(masked):
        # 表示は元ソースから取る。マスクで消えた位置は出さない。
        if hit.index >= len(text) or text[hit.index] not in "+-":
            continue
        line, col = line_col(text, hit.index)
        original_end = hit.index + len(hit.text)
        # 空白を畳んだ表示文字数と元テキストはズレるので、演算子の行だけ出す。
        snippet = text[hit.index:text.find("\n", hit.index)].strip()
        if len(snippet) > 120:
            snippet = snippet[:117] + "..."
        found.append((line, col, Hit(hit.index, hit.kind, hit.text if hit.text else snippet)))
    return found


def lang_of(path: str) -> str:
    ext = os.path.splitext(path)[1].lower()
    if ext == ".razor":
        return "razor"
    if ext == ".cs":
        return "cs"
    return "cpp"


def iter_source_files(root: str) -> list[str]:
    files: list[str] = []
    root = os.path.abspath(root)
    for dirpath, dirnames, filenames in os.walk(root):
        rel = os.path.relpath(dirpath, root)
        parts = [] if rel == "." else rel.split(os.sep)
        if parts and parts[0] in SKIP_TOP:
            dirnames.clear()
            continue
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIR_NAMES and not d.startswith(".")]
        for name in filenames:
            if os.path.splitext(name)[1].lower() in EXTS:
                files.append(os.path.join(dirpath, name))
    files.sort(key=lambda p: p.lower())
    return files


def scan_tree(root: str) -> list[tuple[str, int, int, Hit]]:
    root = os.path.abspath(root)
    rows: list[tuple[str, int, int, Hit]] = []
    for path in iter_source_files(root):
        raw = open(path, "rb").read()
        text = raw.replace(b"\r\n", b"\n").replace(b"\r", b"\n").decode("utf-8-sig", errors="replace")
        # 改行を LF に揃えても、元が CRLF なら列が 1 ずれることはない (CR を除いたため)。
        rel = os.path.relpath(path, root).replace("\\", "/")
        for line, col, hit in scan_text(text, lang_of(path)):
            rows.append((rel, line, col, hit))
    return rows


def _self_test() -> int:
    cpp = r"""
void f(int n) {
    ++pending_;
    --tail;
    for (int i = 0; i < n; ++i) {}
    for (int i = 0; i < n; i++) {}
    for (int i = 0; i < n; --i, ++j) {}
    x = ++i;
    foo(++i);
    a[++i] = 1;
    if (++i) {}
    if (c) ++i;
    return ++i;
    ++a, ++b;
    for (; ++k < n; ) {}
    const char* s = "++not;";
    // ++comment;
    operator++();
    ++obj.member;
    ++arr[i];
    do ++d; while (0);
    a ? b : ++c;
    *++p;
    ++*q;
    i-->0;
    else ++e;
    x = a, ++z;
    for (++init; init < n; ) {}
    1'000;
    const char* raw = R"(
        ++inside_raw;
    )";
    ++after_raw;
}
"""
    expect_cpp = {
        ("文", "++pending_"),
        ("文", "--tail"),
        ("for増分", "++i"),
        ("for増分", "--i"),
        ("for増分", "++j"),
        ("文", "++i"),  # if (c) ++i;
        ("文", "++a"),
        ("文", "++b"),
        ("文", "++obj.member"),
        ("文", "++arr[i]"),
        ("文", "++d"),
        ("文", "++*q"),
        ("文", "++e"),
        ("文", "++z"),
        ("for初期化", "++init"),
        ("文", "++after_raw"),
    }
    got_cpp = {(h.kind, h.text) for _, _, h in scan_text(cpp, "cpp")}
    razor = """
<style>
.a { color: var(--border); }
--name: 1;
</style>
@code {
    void F() {
        ++pending_;
        var reloadVersion = ++_reloadVersion;
        for (int i = 0; i < 3; ++i) { }
        for (int i = 0; i < 3; i++) { }
        return "--:--:--.---";
    }
}
@for (int i = 0; i < n; ++k)
{
    <div class="var(--border)">@i</div>
    ++inBody;
}
"""
    expect_razor = {
        ("文", "++pending_"),
        ("for増分", "++i"),
        ("for増分", "++k"),
        ("文", "++inBody"),
    }
    got_razor = {(h.kind, h.text) for _, _, h in scan_text(razor, "razor")}

    failed = False
    if got_cpp != expect_cpp:
        failed = True
        print("C++ self-test failed", file=sys.stderr)
        print(" missing", sorted(expect_cpp - got_cpp), file=sys.stderr)
        print(" extra  ", sorted(got_cpp - expect_cpp), file=sys.stderr)
    if got_razor != expect_razor:
        failed = True
        print("Razor self-test failed", file=sys.stderr)
        print(" missing", sorted(expect_razor - got_razor), file=sys.stderr)
        print(" extra  ", sorted(got_razor - expect_razor), file=sys.stderr)
    if failed:
        return 1
    print("self-test ok")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="戻り値を使っていない前置 ++/-- を列挙する")
    ap.add_argument("root", nargs="?", help="走査ルート (省略時はリポジトリ直下)")
    ap.add_argument("--self-test", action="store_true", help="埋め込み例で検出条件を確認する")
    args = ap.parse_args()
    if args.self_test:
        return _self_test()

    root = os.path.abspath(args.root) if args.root else os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    rows = scan_tree(root)
    for rel, line, col, hit in rows:
        print(f"{rel}:{line}:{col}: [{hit.kind}] {hit.text}")
    files = len({rel for rel, _, _, _ in rows})
    print(f"合計 {len(rows)} 件 / {files} ファイル")
    return 1 if rows else 0


if __name__ == "__main__":
    raise SystemExit(main())
