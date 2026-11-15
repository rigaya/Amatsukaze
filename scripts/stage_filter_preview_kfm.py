#!/usr/bin/env python3
"""更新済みKFMを完成した配布ツリーのplugins64へ配置する。"""
import argparse
import hashlib
import pathlib
import re
import subprocess
import struct
import shutil
import tempfile
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--plugin", type=pathlib.Path)
    source.add_argument("--url")
    parser.add_argument("--platform", choices=("linux", "windows"), required=True)
    parser.add_argument("--sha256")
    parser.add_argument("--max-glibc", default="2.31", help="Linux配布物で許容するglibcの上限")
    parser.add_argument("--target", required=True, type=pathlib.Path, help="完成した配布ツリーのexe_files/plugins64")
    args = parser.parse_args()
    if not re.fullmatch(r"\d+(?:\.\d+)+", args.max_glibc):
        parser.error("--max-glibcには2.31などの版を指定してください")
    if args.url:
        if not args.sha256:
            parser.error("URLから取得するときは--sha256も指定してください")
        with urllib.request.urlopen(args.url, timeout=60) as response:
            data = response.read()
    else:
        data = args.plugin.read_bytes()
    if args.sha256 and hashlib.sha256(data).hexdigest().lower() != args.sha256.lower():
        parser.error("KFMのSHA-256が一致しません")
    name = "KFM.dll" if args.platform == "windows" else "KFM.so"
    if args.platform == "windows":
        offset = struct.unpack_from("<I", data, 0x3c)[0] if len(data) >= 0x40 else -1
        valid = offset >= 0 and len(data) >= offset + 6 and data.startswith(b"MZ") \
            and data[offset:offset + 4] == b"PE\x00\x00" and struct.unpack_from("<H", data, offset + 4)[0] == 0x8664
    else:
        valid = len(data) >= 20 and data[:6] == b"\x7fELF\x02\x01" and struct.unpack_from("<H", data, 18)[0] == 62
    if not valid or any(marker not in data for marker in (b"KFM_FrameType", b"KFM_Duration")):
        parser.error(f"{args.platform}のx64用でKFM_FrameTypeとKFM_Durationを持つ更新版を指定してください")
    args.target.mkdir(parents=True, exist_ok=True)
    # 完全に書き終えたファイルを置き換え、途中の共有ライブラリを残さない。
    with tempfile.TemporaryDirectory(prefix="kfm-stage-", dir=args.target) as temporary:
        staged = pathlib.Path(temporary) / name
        staged.write_bytes(data)
        staged.chmod(0o755)
        if name == "KFM.so":
            # ベース配布物のABI要件を超えるローカルビルドを混入させない。
            symbols = subprocess.check_output(["objdump", "-p", str(staged)], text=True)
            versions = [tuple(map(int, version.split("."))) for version in re.findall(r"\bGLIBC_(\d+(?:\.\d+)+)", symbols)]
            limit = tuple(map(int, args.max_glibc.split(".")))
            if versions and max(versions) > limit:
                parser.error(f"KFMが要求するglibc{'.'.join(map(str, max(versions)))}は配布上限{args.max_glibc}を超えています")
        shutil.move(str(staged), args.target / name)
    print(f"更新版KFMを配置しました: {args.target / name} SHA-256={hashlib.sha256(data).hexdigest()}")


if __name__ == "__main__":
    main()
