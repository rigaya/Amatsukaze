#!/usr/bin/env python3
"""配布するフィルタプレビューエンジンの起動とプロトコルを確認する。"""
import pathlib
import subprocess
import sys
import tempfile


def main():
    if len(sys.argv) != 2:
        raise ValueError("確認するHWEncVppPreviewのパスを指定してください")
    executable = pathlib.Path(sys.argv[1]).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="amatsukaze-preview-") as temporary:
        result = subprocess.run(
            [str(executable), "--amatsukaze-engine", "--tmp", temporary,
             "--video", "0", "--amatsukaze-dir", str(executable.parent)],
            input=b'{"cmd":"quit"}\n', capture_output=True, timeout=30,
        )
    lines = result.stdout.decode("utf-8-sig", errors="replace").splitlines()
    if result.returncode != 0 or not lines or lines[0] != "AMTPREVIEW READY version=1":
        raise RuntimeError("プレビューエンジンの起動またはプロトコル確認に失敗しました: "
                           + repr(lines[:1]) + " " + result.stderr.decode("utf-8", errors="replace"))
    print("フィルタプレビューエンジンの起動・READY version=1確認:成功")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
