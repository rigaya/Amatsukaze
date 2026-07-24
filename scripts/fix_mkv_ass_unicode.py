#!/usr/bin/env python3

"""MKV内のASS字幕にあるUnicode字間問題を修正し、元ファイルと入れ替える。"""

from __future__ import annotations

import json
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from copy import deepcopy
from pathlib import Path


# 環境に合わせて変更する。PATHが通っていないWindows環境では、例えば
# r"C:\Program Files\MKVToolNix\mkvextract.exe" のようにフルパスを指定する。
MKVEXTRACT_PATH = r"mkvextract"
MKVMERGE_PATH = r"mkvmerge"

FIXED_SUFFIX = "_fixed"
BACKUP_SUFFIX = "_orig"
FIX_OLD_IVS_SCALE = True

STATISTICS_TAG_NAMES = {
    "BPS",
    "DURATION",
    "NUMBER_OF_FRAMES",
    "NUMBER_OF_BYTES",
    "_STATISTICS_WRITING_APP",
    "_STATISTICS_WRITING_DATE_UTC",
    "_STATISTICS_TAGS",
}

NUMBER_PATTERN = r"[+-]?(?:\d+(?:\.\d*)?|\.\d+)"
SPACING_PATTERN = re.compile(rf"\\fsp(?P<value>{NUMBER_PATTERN})", re.IGNORECASE)
OLD_IVS_SCALE_PATTERN = re.compile(
    rf"(?P<tag_start>\{{[^{{}}\r\n]*?\\fscx)(?P<old>{NUMBER_PATTERN})"
    rf"(?P<tag_end>[^{{}}\r\n]*\}})"
    rf"(?P<prefix>[^{{}}\r\n]?)"
    rf"(?:(?P<zero_start>\{{[^{{}}\r\n]*?\\fsp)(?P<zero>{NUMBER_PATTERN})"
    rf"(?P<zero_end>[^{{}}\r\n]*\}}))?"
    rf"(?P<base>[^{{}}\r\n])"
    rf"(?P<selector>[\u180B-\u180D\u180F\uFE00-\uFE0F\U000E0100-\U000E01EF])"
    rf"(?:(?P<spacing_start>\{{[^{{}}\r\n]*?\\fsp)(?P<spacing>{NUMBER_PATTERN})"
    rf"(?P<spacing_end>[^{{}}\r\n]*\}}))?"
    rf"(?P<reset_start>\{{[^{{}}\r\n]*?\\fscx)(?P<new>{NUMBER_PATTERN})"
    rf"(?P<reset_end>[^{{}}\r\n]*\}})",
    re.IGNORECASE,
)
OLD_IVS_TERMINAL_SCALE_PATTERN = re.compile(
    rf"(?P<tag_start>\{{[^{{}}\r\n]*?\\fscx)(?P<old>{NUMBER_PATTERN})"
    rf"(?P<tag_end>[^{{}}\r\n]*\}})"
    rf"(?P<base>[^{{}}\r\n])"
    rf"(?P<selector>[\u180B-\u180D\u180F\uFE00-\uFE0F\U000E0100-\U000E01EF])$",
    re.IGNORECASE,
)


def run_tool(
    command: list[str],
    capture_output: bool = False,
    allow_warning: bool = True,
) -> str:
    result = subprocess.run(
        command,
        check=False,
        text=True,
        encoding="utf-8",
        errors="replace",
        stdout=subprocess.PIPE if capture_output else None,
    )
    # MKVToolNixは警告がある場合に1を返す。2以上だけを失敗として扱う。
    if result.returncode >= 2:
        raise RuntimeError(f"コマンドが失敗しました ({result.returncode}): {' '.join(command)}")
    if result.returncode == 1 and not allow_warning:
        raise RuntimeError(f"コマンドが警告終了しました: {' '.join(command)}")
    if result.returncode == 1:
        print(f"警告付きで完了しました: {' '.join(command)}", file=sys.stderr)
    return result.stdout or ""


def identify_mkv(input_path: Path) -> dict:
    output = run_tool([MKVMERGE_PATH, "-J", str(input_path)], capture_output=True)
    return json.loads(output)


def is_variation_selector(char: str) -> bool:
    value = ord(char)
    return (
        0x180B <= value <= 0x180D
        or value == 0x180F
        or 0xFE00 <= value <= 0xFE0F
        or 0xE0100 <= value <= 0xE01EF
    )


def fix_old_ivs_scale(text: str) -> tuple[str, int]:
    if not FIX_OLD_IVS_SCALE:
        return text, 0

    count = 0

    def replace(match: re.Match[str]) -> str:
        nonlocal count
        old_scale = float(match.group("old"))
        restored_scale = float(match.group("new"))
        # 旧版AmatsukazeではIVSを構成する異体字セレクタを文字数に含めたため、
        # 書式の切り替え位置が1表示文字ぶん後ろへずれる場合がある。実例では
        # 「{\fscx50}　逢󠄁{\fscx100}田さんは」となり、「逢󠄁」まで半幅で表示された。
        # 復帰値が直前の値のちょうど2倍で、復帰タグがIVS直後にある場合に限り、
        # 復帰タグをIVS直前へ移して旧版による境界ずれを修正する。旧スクリプトが
        # IVS前後へ追加済みの\fsp0と復帰タグがある場合は、それらを保ったまま移動する。
        if abs(restored_scale - old_scale * 2) > 0.001:
            return match.group(0)
        if match.group("zero") is not None:
            if not spacing_is_zero(match.group("zero")) or match.group("spacing") is None:
                return match.group(0)
        elif match.group("spacing") is not None:
            return match.group(0)
        count += 1
        return (
            match.group("tag_start")
            + match.group("old")
            + match.group("tag_end")
            + match.group("prefix")
            + match.group("reset_start")
            + match.group("new")
            + match.group("reset_end")
            + (match.group("zero_start") or "")
            + (match.group("zero") or "")
            + (match.group("zero_end") or "")
            + match.group("base")
            + match.group("selector")
            + (match.group("spacing_start") or "")
            + (match.group("spacing") or "")
            + (match.group("spacing_end") or "")
        )

    text = OLD_IVS_SCALE_PATTERN.sub(replace, text)

    def replace_terminal(match: re.Match[str]) -> str:
        nonlocal count
        old_scale = float(match.group("old"))
        # 復帰タグがない行末では断定材料が少ないため、旧標準出力で生じる50以下の半幅値に限定する。
        if old_scale > 50:
            return match.group(0)
        corrected_scale = old_scale * 2
        corrected_text = f"{corrected_scale:g}"
        count += 1
        return (
            match.group("tag_start")
            + corrected_text
            + match.group("tag_end")
            + match.group("base")
            + match.group("selector")
        )

    return OLD_IVS_TERMINAL_SCALE_PATTERN.sub(replace_terminal, text), count


def spacing_is_zero(spacing: str) -> bool:
    try:
        return abs(float(spacing)) < 0.000001
    except ValueError:
        return False


def fix_dialogue_text(text: str, initial_spacing: str) -> tuple[str, int, int]:
    text, scale_fix_count = fix_old_ivs_scale(text)
    output: list[str] = []
    spacing = initial_spacing
    spacing_fix_count = 0
    pos = 0

    while pos < len(text):
        if text[pos] == "{":
            end = text.find("}", pos + 1)
            if end >= 0:
                override = text[pos : end + 1]
                output.append(override)
                for match in SPACING_PATTERN.finditer(override):
                    spacing = match.group("value")
                pos = end + 1
                continue

        # \N、\n、\hなどのASSエスケープは2文字一組のまま扱う。
        if text[pos] == "\\" and pos + 1 < len(text):
            output.append(text[pos : pos + 2])
            pos += 2
            continue

        end = pos + 1
        requires_zero_spacing = ord(text[pos]) >= 0x10000 or is_variation_selector(text[pos])
        if not is_variation_selector(text[pos]) and end < len(text) and is_variation_selector(text[end]):
            end += 1
            requires_zero_spacing = True

        character = text[pos:end]
        if requires_zero_spacing and not spacing_is_zero(spacing):
            output.extend(("{\\fsp0}", character, f"{{\\fsp{spacing}}}"))
            spacing_fix_count += 1
        else:
            output.append(character)
        pos = end

    return "".join(output), spacing_fix_count, scale_fix_count


def read_style_spacings(lines: list[str]) -> dict[str, str]:
    section = ""
    format_fields: list[str] = []
    spacings: dict[str, str] = {}

    for line in lines:
        stripped = line.lstrip("\ufeff").strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            section = stripped.casefold()
            continue
        if section != "[v4+ styles]":
            continue
        if stripped.casefold().startswith("format:"):
            format_fields = [field.strip().casefold() for field in stripped.split(":", 1)[1].split(",")]
            continue
        if not stripped.casefold().startswith("style:") or not format_fields:
            continue

        values = [value.strip() for value in stripped.split(":", 1)[1].split(",", len(format_fields) - 1)]
        if len(values) != len(format_fields):
            continue
        fields = dict(zip(format_fields, values))
        if "name" in fields:
            spacings[fields["name"]] = fields.get("spacing", "0") or "0"

    return spacings


def fix_ass_file(path: Path, track_id: int) -> bool:
    raw = path.read_bytes()
    has_bom = raw.startswith(b"\xef\xbb\xbf")
    text = raw.decode("utf-8-sig")
    lines = text.splitlines(keepends=True)
    style_spacings = read_style_spacings(lines)
    modified = False
    total_spacing_fixes = 0
    total_scale_fixes = 0
    output_lines: list[str] = []

    for line_number, line in enumerate(lines, 1):
        body = line.rstrip("\r\n")
        line_end = line[len(body) :]
        if not body.casefold().startswith("dialogue:"):
            output_lines.append(line)
            continue

        fields = body.split(",", 9)
        if len(fields) != 10:
            output_lines.append(line)
            continue

        style = fields[3].strip()
        spacing = style_spacings.get(style, "0")
        fixed_text, spacing_fixes, scale_fixes = fix_dialogue_text(fields[9], spacing)
        if fixed_text == fields[9]:
            output_lines.append(line)
            continue

        fixed_body = ",".join(fields[:9] + [fixed_text])
        output_lines.append(fixed_body + line_end)
        modified = True
        total_spacing_fixes += spacing_fixes
        total_scale_fixes += scale_fixes
        print(f"[ASSトラック {track_id} / {line_number}行目]")
        print(f"- {body}")
        print(f"+ {fixed_body}")

    if not modified:
        return False

    encoded = "".join(output_lines).encode("utf-8")
    path.write_bytes((b"\xef\xbb\xbf" if has_bom else b"") + encoded)
    print(
        f"ASSトラック {track_id}: 字間修正 {total_spacing_fixes}箇所、"
        f"旧IVS横幅修正 {total_scale_fixes}箇所"
    )
    return True


def replacement_track_options(track: dict) -> list[str]:
    properties = track.get("properties", {})
    options = ["--sub-charset", "0:UTF-8"]
    language = properties.get("language_ietf") or properties.get("language")
    if language:
        options.extend(("--language", f"0:{language}"))
    if "track_name" in properties:
        options.extend(("--track-name", f"0:{properties['track_name']}"))

    flag_options = {
        "default_track": "--default-track-flag",
        "forced_track": "--forced-display-flag",
        "enabled_track": "--track-enabled-flag",
        "hearing_impaired": "--hearing-impaired-flag",
        "visual_impaired": "--visual-impaired-flag",
        "text_descriptions": "--text-descriptions-flag",
        "original": "--original-flag",
        "commentary": "--commentary-flag",
    }
    for property_name, option_name in flag_options.items():
        if property_name in properties:
            value = 1 if properties[property_name] else 0
            options.extend((option_name, f"0:{value}"))
    return options


def make_track_tag_files(
    input_path: Path,
    identification: dict,
    track_ids: set[int],
    temp_path: Path,
) -> dict[int, Path]:
    tagged_track_ids = {
        entry.get("track_id") for entry in identification.get("track_tags", [])
    } & track_ids
    if not tagged_track_ids:
        return {}

    all_tags_path = temp_path / "all-tags.xml"
    run_tool([MKVEXTRACT_PATH, "-q", str(input_path), "tags", str(all_tags_path)])
    root = ET.parse(all_tags_path).getroot()
    uid_by_track_id = {
        track["id"]: str(track.get("properties", {}).get("uid"))
        for track in identification.get("tracks", [])
        if track["id"] in tagged_track_ids
    }
    result: dict[int, Path] = {}

    for track_id, track_uid in uid_by_track_id.items():
        output_root = ET.Element("Tags")
        for tag in root.findall("Tag"):
            target_uids = [element.text for element in tag.findall("./Targets/TrackUID")]
            if track_uid not in target_uids:
                continue

            copied_tag = deepcopy(tag)
            targets = copied_tag.find("Targets")
            if targets is not None:
                for uid_element in targets.findall("TrackUID"):
                    targets.remove(uid_element)
            for simple in list(copied_tag.findall("Simple")):
                name = simple.findtext("Name", default="")
                if name in STATISTICS_TAG_NAMES:
                    copied_tag.remove(simple)
            if copied_tag.findall("Simple"):
                output_root.append(copied_tag)

        if not list(output_root):
            continue
        output_path = temp_path / f"track-{track_id}-tags.xml"
        ET.ElementTree(output_root).write(output_path, encoding="utf-8", xml_declaration=True)
        result[track_id] = output_path

    return result


def remux_mkv(
    input_path: Path,
    output_path: Path,
    identification: dict,
    replacements: dict[int, Path],
    track_tag_files: dict[int, Path],
) -> None:
    tracks = identification.get("tracks", [])
    replacement_file_ids = {
        track_id: file_id for file_id, track_id in enumerate(replacements, start=1)
    }
    track_order = []
    for track in tracks:
        track_id = track["id"]
        if track_id in replacement_file_ids:
            track_order.append(f"{replacement_file_ids[track_id]}:0")
        else:
            track_order.append(f"0:{track_id}")

    retained_subtitle_ids = [
        track["id"]
        for track in tracks
        if track.get("type") == "subtitles" and track["id"] not in replacements
    ]
    command = [MKVMERGE_PATH, "-q", "-o", str(output_path), "--track-order", ",".join(track_order)]
    if retained_subtitle_ids:
        command.extend(("--subtitle-tracks", ",".join(map(str, retained_subtitle_ids))))
    else:
        command.append("--no-subtitles")
    command.append(str(input_path))

    tracks_by_id = {track["id"]: track for track in tracks}
    for track_id, ass_path in replacements.items():
        command.extend(replacement_track_options(tracks_by_id[track_id]))
        if track_id in track_tag_files:
            command.extend(("--tags", f"0:{track_tag_files[track_id]}"))
        command.append(str(ass_path))

    # 元ファイルを入れ替えるのは、mkvmergeが警告なしの終了コード0で完了した場合だけとする。
    run_tool(command, allow_warning=False)


def install_fixed_mkv(input_path: Path, fixed_path: Path, backup_path: Path) -> None:
    input_path.rename(backup_path)
    try:
        fixed_path.rename(input_path)
    except OSError:
        # 修正済みファイルの移動に失敗した場合は、退避した元ファイルを元の名前へ戻す。
        if not input_path.exists() and backup_path.exists():
            backup_path.rename(input_path)
        raise


def main() -> int:
    if len(sys.argv) != 2:
        print(f"使い方: {Path(sys.argv[0]).name} 入力.mkv", file=sys.stderr)
        return 2

    input_path = Path(sys.argv[1]).expanduser().resolve()
    if not input_path.is_file() or input_path.suffix.casefold() != ".mkv":
        print(f"MKVファイルが見つかりません: {input_path}", file=sys.stderr)
        return 2

    fixed_path = input_path.with_name(input_path.stem + FIXED_SUFFIX + input_path.suffix)
    backup_path = input_path.with_name(input_path.stem + BACKUP_SUFFIX + input_path.suffix)

    try:
        identification = identify_mkv(input_path)
        ass_tracks = [
            track
            for track in identification.get("tracks", [])
            if track.get("properties", {}).get("codec_id") == "S_TEXT/ASS"
        ]
        if not ass_tracks:
            print("ASS字幕トラックはありません。")
            return 0

        with tempfile.TemporaryDirectory(prefix="amatsukaze-fix-ass-") as temp_dir:
            temp_path = Path(temp_dir)
            extracted = {
                track["id"]: temp_path / f"track-{track['id']}.ass" for track in ass_tracks
            }
            extract_command = [MKVEXTRACT_PATH, "-q", str(input_path), "tracks"]
            extract_command.extend(f"{track_id}:{path}" for track_id, path in extracted.items())
            run_tool(extract_command)

            replacements = {
                track_id: path
                for track_id, path in extracted.items()
                if fix_ass_file(path, track_id)
            }
            if not replacements:
                print("修正が必要なASS字幕はありません。")
                return 0
            if fixed_path.exists():
                print(f"一時出力先がすでに存在します: {fixed_path}", file=sys.stderr)
                return 2
            if backup_path.exists():
                print(f"退避先がすでに存在します: {backup_path}", file=sys.stderr)
                return 2

            track_tag_files = make_track_tag_files(
                input_path,
                identification,
                set(replacements),
                temp_path,
            )
            remux_mkv(
                input_path,
                fixed_path,
                identification,
                replacements,
                track_tag_files,
            )

        install_fixed_mkv(input_path, fixed_path, backup_path)
        print(f"元MKVを退避しました: {backup_path}")
        print(f"修正済みMKVへ入れ替えました: {input_path}")
        return 0
    except (OSError, RuntimeError, json.JSONDecodeError, UnicodeError) as error:
        # 元ファイルが正しい名前で残っている場合だけ、失敗した一時出力を削除する。
        if input_path.exists() and fixed_path.exists():
            fixed_path.unlink()
        print(f"エラー: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
