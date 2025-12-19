#!/usr/bin/env python3
"""Convert pre-increment in for-loop increment clauses.

Scope:
- Only targets C/C++ source files under the given root directory.
- Only rewrites the 3rd expression (increment clause) of a for-loop header.
- Rewrites '++var' -> 'var++' (where var is an identifier).

This is a best-effort text transformation with lightweight parsing to avoid
rewriting inside strings and to keep the change localized.

Implementation notes:
- Preserves the original file's dominant line ending style (CRLF vs LF).
- Preserves the original whitespace between `for` and `(` by reusing the
  original prefix text.
"""

from __future__ import annotations

import argparse
import os
import re
from dataclasses import dataclass


EXTS = {".cpp", ".h", ".hpp"}


def find_matching_paren(s: str, start: int) -> int:
    """Return index of matching ')' for s[start] == '(', or -1."""
    depth = 0
    i = start
    in_str: str | None = None
    esc = False

    while i < len(s):
        c = s[i]

        if in_str is not None:
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == in_str:
                in_str = None
            i += 1
            continue

        if c == '"' or c == "'":
            in_str = c
            i += 1
            continue

        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1

    return -1


def split_top_level(s: str, sep: str = ";") -> list[str]:
    """Split s by sep only at top-level (not inside ()[]{} or strings).

    Note: We intentionally do NOT treat '<' / '>' as nesting because they are
    ubiquitous in comparisons (e.g. `i < n`) inside for-headers.
    """
    parts: list[str] = []
    cur: list[str] = []

    par = brk = brc = 0
    in_str: str | None = None
    esc = False

    i = 0
    while i < len(s):
        c = s[i]

        if in_str is not None:
            cur.append(c)
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == in_str:
                in_str = None
            i += 1
            continue

        if c == '"' or c == "'":
            in_str = c
            cur.append(c)
            i += 1
            continue

        if c == "(":
            par += 1
        elif c == ")":
            par = max(0, par - 1)
        elif c == "[":
            brk += 1
        elif c == "]":
            brk = max(0, brk - 1)
        elif c == "{":
            brc += 1
        elif c == "}":
            brc = max(0, brc - 1)

        if c == sep and par == 0 and brk == 0 and brc == 0:
            parts.append("".join(cur))
            cur = []
            i += 1
            continue

        cur.append(c)
        i += 1

    parts.append("".join(cur))
    return parts


_PREINC_IDENT = re.compile(r"(?<!\+)\+\+\s*([A-Za-z_]\w*)")


def convert_third_expr(third: str) -> tuple[str, int]:
    """Convert ++var -> var++ in a for-loop increment expression."""
    n = 0

    def _repl(m: re.Match[str]) -> str:
        nonlocal n
        n += 1
        return f"{m.group(1)}++"

    new = _PREINC_IDENT.sub(_repl, third)
    return new, n


@dataclass
class FileChange:
    path: str
    num_for_headers_changed: int
    num_replacements: int


def process_text(text: str) -> tuple[str, int, int]:
    out: list[str] = []
    i = 0
    for_headers_changed = 0
    replacements = 0

    for_pat = re.compile(r"\bfor\b")

    while True:
        m = for_pat.search(text, i)
        if not m:
            out.append(text[i:])
            break

        out.append(text[i : m.start()])

        j = m.end()
        while j < len(text) and text[j].isspace():
            j += 1

        # Not a 'for (...)'
        if j >= len(text) or text[j] != "(":
            out.append(text[m.start() : j])
            i = j
            continue

        # Preserve original `for` ... `(` (including whitespace)
        for_prefix = text[m.start() : j + 1]

        end = find_matching_paren(text, j)
        if end < 0:
            out.append(text[m.start() :])
            break

        header = text[j + 1 : end]
        parts = split_top_level(header, ";")

        if len(parts) == 3:
            third = parts[2]
            new_third, n = convert_third_expr(third)
            if n and new_third != third:
                parts[2] = new_third
                new_header = ";".join(parts)
                out.append(for_prefix + new_header + ")")
                for_headers_changed += 1
                replacements += n
            else:
                out.append(text[m.start() : end + 1])
            i = end + 1
        else:
            # Leave non-standard for headers untouched (e.g. range-for)
            out.append(text[m.start() : end + 1])
            i = end + 1

    return "".join(out), for_headers_changed, replacements


def iter_target_files(root: str) -> list[str]:
    files: list[str] = []
    for dp, _, fn in os.walk(root):
        for f in fn:
            if os.path.splitext(f)[1].lower() in EXTS:
                files.append(os.path.join(dp, f))
    files.sort(key=lambda p: p.lower())
    return files


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("root", help="target directory (e.g. Amatsukaze)")
    ap.add_argument("--dry-run", action="store_true", help="do not write files")
    args = ap.parse_args()

    root = os.path.abspath(args.root)

    changes: list[FileChange] = []

    for path in iter_target_files(root):
        raw = open(path, "rb").read()
        had_crlf = b"\r\n" in raw
        # Normalize for processing
        txt = raw.replace(b"\r\n", b"\n").decode("utf-8", errors="surrogateescape")

        new, for_changed, reps = process_text(txt)
        if reps and new != txt:
            if not args.dry_run:
                out_txt = new
                if had_crlf:
                    out_txt = out_txt.replace("\n", "\r\n")
                out_raw = out_txt.encode("utf-8", errors="surrogateescape")
                open(path, "wb").write(out_raw)
            changes.append(FileChange(path=path, num_for_headers_changed=for_changed, num_replacements=reps))

    print(f"modified_files {len(changes)}")
    for ch in changes:
        rel = os.path.relpath(ch.path, root)
        print(f"{rel}: for_headers={ch.num_for_headers_changed}, replacements={ch.num_replacements}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
