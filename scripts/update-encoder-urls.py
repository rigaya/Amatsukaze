#!/usr/bin/env python3
# エンコーダ関連のURL・バージョンを最新リリースから取得して更新するスクリプト
#
# 更新対象:
#   - .github/workflows/build_windows_package.yml
#       X264_URL / X265_URL / SVT_URL (rigaya/AutoBuildForAviUtlPlugins の最新リリース)
#       ※ LSMASH_URL / X262_URL は最新リリースに該当アセットが無いため更新対象外
#   - docker/Dockerfile
#       ARG QSVENCC_VER / NVENCC_VER / VCEENCC_VER (各エンコーダdebの最新リリースタグ)
#       ※ Linux版x264/x265/SVT-AV1は配布アーカイブ(basepkg)に含まれるためDockerfile側にはURL不存在
import requests
import re
import sys
import subprocess
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent

WIN_PACKAGE_ENV_VARS = (
    "X264_URL",
    "X265_URL",
    "SVT_URL",
    "BASE_PKG_URL",
)

# Dockerfileでバージョン管理しているエンコーダdeb (ENVではなくARGで定義されている)
DOCKER_ENCODER_ARGS = {
    "QSVENCC_VER": "rigaya/QSVEnc",
    "NVENCC_VER": "rigaya/NVEnc",
    "VCEENCC_VER": "rigaya/VCEEnc",
}

def get_latest_release(repo):
    url = f"https://api.github.com/repos/{repo}/releases/latest"
    response = requests.get(url, timeout=30)
    response.raise_for_status()
    return response.json()

def repo_path(path):
    return REPO_ROOT / path

def line_ending(text):
    return "\r\n" if "\r\n" in text else "\n"

def find_job_env_block(lines, job_name):
    job_pattern = re.compile(rf"^(?P<indent>\s*){re.escape(job_name)}:\s*(?:#.*)?$")
    for job_index, line in enumerate(lines):
        match = job_pattern.match(line.rstrip("\r\n"))
        if not match:
            continue

        job_indent = len(match.group("indent"))
        for env_index in range(job_index + 1, len(lines)):
            stripped = lines[env_index].strip()
            if not stripped or stripped.startswith("#"):
                continue

            line_indent = len(lines[env_index]) - len(lines[env_index].lstrip(" "))
            if line_indent <= job_indent:
                break

            if stripped == "env:":
                env_indent = line_indent
                end_index = len(lines)
                for index in range(env_index + 1, len(lines)):
                    current = lines[index]
                    current_stripped = current.strip()
                    if not current_stripped or current_stripped.startswith("#"):
                        continue
                    current_indent = len(current) - len(current.lstrip(" "))
                    if current_indent <= env_indent:
                        end_index = index
                        break
                return env_index, end_index, env_indent

    raise ValueError(f"Could not find env block for job '{job_name}'")

def infer_env_entry_indent(lines, env_start, env_end, env_indent):
    for line in lines[env_start + 1:env_end]:
        body = line.rstrip("\r\n")
        if not body.strip() or body.lstrip().startswith("#"):
            continue
        match = re.match(r"^(\s*)[A-Za-z_][A-Za-z0-9_]*:\s*", body)
        if match:
            return match.group(1)
    return " " * (env_indent + 2)

def update_job_env_var(content, job_name, var_name, value):
    lines = content.splitlines(keepends=True)
    newline = line_ending(content)
    env_start, env_end, env_indent = find_job_env_block(lines, job_name)
    entry_indent = infer_env_entry_indent(lines, env_start, env_end, env_indent)
    pattern = re.compile(rf"^(\s*){re.escape(var_name)}:\s*.*?(\r?\n)?$")

    for index in range(env_start + 1, env_end):
        if pattern.match(lines[index]):
            current_newline = "\r\n" if lines[index].endswith("\r\n") else "\n"
            lines[index] = f"{entry_indent}{var_name}: {value}{current_newline}"
            return "".join(lines)

    lines.insert(env_end, f"{entry_indent}{var_name}: {value}{newline}")
    return "".join(lines)

def validate_win_package_env(content):
    lines = content.splitlines()
    env_start, env_end, env_indent = find_job_env_block(lines, "build-windows")
    entry_indent = infer_env_entry_indent(lines, env_start, env_end, env_indent)
    expected_indent = len(entry_indent)
    found = {}

    for line in lines[env_start + 1:env_end]:
        match = re.match(r"^(\s*)([A-Za-z_][A-Za-z0-9_]*):\s*(.*)$", line)
        if not match:
            continue
        indent, name, value = match.groups()
        if name in WIN_PACKAGE_ENV_VARS:
            found[name] = value
            if len(indent) != expected_indent:
                raise ValueError(f"{name} has invalid indentation in build_windows_package.yml")
            if not value:
                raise ValueError(f"{name} is empty in build_windows_package.yml")

    missing = [name for name in WIN_PACKAGE_ENV_VARS if name not in found]
    if missing:
        raise ValueError(f"Missing env var(s) in build_windows_package.yml: {', '.join(missing)}")

def write_if_changed(path, content, new_content):
    if new_content != content:
        # newline='' で改行コード (CRLF等) を変換せずそのまま書き戻す
        with open(path, 'w', encoding='utf-8', newline='') as f:
            f.write(new_content)
        print(f"Updated {path}")
    else:
        print(f"No changes for {path}")

def read_preserving_newlines(path):
    # newline='' で読み、各行の改行コードを保持したまま取得する
    with open(path, 'r', encoding='utf-8', newline='') as f:
        return f.read()

def update_workflow_env_var(file_path, var_name, value):
    path = repo_path(file_path)
    if not path.exists():
        print(f"Error: File {path} not found")
        return

    content = read_preserving_newlines(path)
    new_content = update_job_env_var(content, "build-windows", var_name, value)
    write_if_changed(path, content, new_content)

def update_dockerfile_arg(file_path, arg_name, value):
    path = repo_path(file_path)
    if not path.exists():
        print(f"Error: File {path} not found")
        return

    content = read_preserving_newlines(path)

    pattern = rf"^ARG {re.escape(arg_name)}=[^ \n\r]+"
    replacement = f"ARG {arg_name}={value}"
    new_content, count = re.subn(pattern, replacement, content, flags=re.MULTILINE)
    if count == 0:
        raise ValueError(f"Pattern not found in {path}: {pattern}")
    write_if_changed(path, content, new_content)

def update_if_value(new_value, description, updater, *args):
    if not new_value:
        print(f"Warning: {description} not found. Keeping existing value.")
        return False

    try:
        updater(*args)
        return True
    except Exception as e:
        print(f"Warning: Failed to update {description}: {e}. Keeping existing value.")
        return False

def find_asset(assets, *tokens):
    for asset in assets:
        name = asset['name']
        if all(token in name for token in tokens):
            return asset['browser_download_url']
    return None

def verify_changes():
    win_yml = repo_path(".github/workflows/build_windows_package.yml")
    with open(win_yml, 'r', encoding='utf-8') as f:
        validate_win_package_env(f.read())

    print("--- Verification ---", flush=True)
    subprocess.run([
        "rg", "-n",
        "x264|x265|svt|SvtAv1|QSVENCC_VER|NVENCC_VER|VCEENCC_VER|BASE_PKG_URL",
        ".github/workflows/build_windows_package.yml",
        "docker/Dockerfile",
    ], cwd=REPO_ROOT, check=True)

def main():
    try:
        dockerfile = "docker/Dockerfile"
        win_yml = ".github/workflows/build_windows_package.yml"

        # 1. AutoBuildForAviUtlPlugins → WindowsパッケージのエンコーダURL
        try:
            print("Fetching AutoBuildForAviUtlPlugins latest release...")
            ab_release = get_latest_release("rigaya/AutoBuildForAviUtlPlugins")
            assets = ab_release['assets']

            for asset in assets:
                print(f"  Asset found: {asset['name']}")

            x264_win = find_asset(assets, "x264", "x64.zip")
            x265_win = find_asset(assets, "x265", "x64.zip")
            svt_win = find_asset(assets, "SvtAv1EncApp", "x64_clang.zip")
        except Exception as e:
            print(f"Warning: Failed to fetch AutoBuildForAviUtlPlugins latest release: {e}. Keeping existing AutoBuild URLs.")
            x264_win = x265_win = svt_win = None

        update_if_value(x264_win, "Windows x264 URL", update_workflow_env_var, win_yml, "X264_URL", x264_win)
        update_if_value(x265_win, "Windows x265 URL", update_workflow_env_var, win_yml, "X265_URL", x265_win)
        update_if_value(svt_win, "Windows SVT-AV1 URL", update_workflow_env_var, win_yml, "SVT_URL", svt_win)

        # 2. Dockerfileのエンコーダdebバージョン (ARG)
        for arg_name, repo in DOCKER_ENCODER_ARGS.items():
            try:
                print(f"Fetching {repo} latest release...")
                release = get_latest_release(repo)
                ver = release['tag_name']
                update_if_value(ver, arg_name, update_dockerfile_arg, dockerfile, arg_name, ver)
            except Exception as e:
                print(f"Warning: Failed to update {arg_name} from {repo}: {e}. Keeping existing value.")

        # 3. Verification
        print("Verifying changes...")
        verify_changes()

    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
