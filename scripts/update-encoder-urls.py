#!/usr/bin/env python3
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

def update_file(file_path, pattern, replacement, is_env_var=False, var_name=None, value=None):
    path = repo_path(file_path)
    if not path.exists():
        print(f"Error: File {path} not found")
        return
    
    with open(path, 'r', encoding='utf-8') as f:
        content = f.read()
    
    if is_env_var:
        new_content = update_job_env_var(content, "build-windows", var_name, value)
    else:
        new_content, count = re.subn(pattern, replacement, content, flags=re.MULTILINE)
        if count == 0:
            raise ValueError(f"Pattern not found in {path}: {pattern}")
    
    if new_content != content:
        with open(path, 'w', encoding='utf-8') as f:
            f.write(new_content)
        print(f"Updated {path}")
    else:
        print(f"No changes for {path}")

def update_file_if_value(new_value, description, *args, **kwargs):
    if not new_value:
        print(f"Warning: {description} not found. Keeping existing value.")
        return False

    try:
        update_file(*args, **kwargs)
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
        "x264|x265|svt-av1|SvtAv1EncApp|AutoBuildForAviUtlPlugins|QSVENCC_VER|NVENCC_VER|VCEENCC_VER|TSREPLACE_VER|QSVEnc|NVEnc|VCEEnc|tsreplace|BASE_PKG_URL",
        ".github/workflows/build_windows_package.yml",
        "docker/Dockerfile",
    ], cwd=REPO_ROOT, check=True)

def main():
    try:
        # 1. AutoBuildForAviUtlPlugins
        x264_win = x265_win = svt_win = None
        x264_linux = x265_linux = svt_linux = None
        dockerfile = "docker/Dockerfile"
        win_yml = ".github/workflows/build_windows_package.yml"

        try:
            print("Fetching AutoBuildForAviUtlPlugins latest release...")
            ab_release = get_latest_release("rigaya/AutoBuildForAviUtlPlugins")
            assets = ab_release['assets']

            for asset in assets:
                name = asset['name']
                print(f"  Asset found: {name}")

            x264_win = find_asset(assets, "x264", "x64.zip")
            x265_win = find_asset(assets, "x265", "x64.zip")
            svt_win = find_asset(assets, "SvtAv1EncApp", "x64_clang.zip")
            x264_linux = find_asset(assets, "x264", "amd64_linux.tar.xz")
            x265_linux = find_asset(assets, "x265", "amd64_linux.tar.xz")
            svt_linux = find_asset(assets, "SvtAv1EncApp", "amd64_linux_clang.tar.xz")
        except Exception as e:
            print(f"Warning: Failed to fetch AutoBuildForAviUtlPlugins latest release: {e}. Keeping existing AutoBuild URLs.")

        # Update .github/workflows/build_windows_package.yml
        update_file_if_value(x264_win, "Windows x264 URL", win_yml, r"^\s*X264_URL: .*", f"X264_URL: {x264_win}", is_env_var=True, var_name="X264_URL", value=x264_win)
        update_file_if_value(x265_win, "Windows x265 URL", win_yml, r"^\s*X265_URL: .*", f"X265_URL: {x265_win}", is_env_var=True, var_name="X265_URL", value=x265_win)
        update_file_if_value(svt_win, "Windows SVT-AV1 URL", win_yml, r"^\s*SVT_URL: .*", f"SVT_URL: {svt_win}", is_env_var=True, var_name="SVT_URL", value=svt_win)

        # Update docker/Dockerfile (AutoBuildForAviUtlPlugins part)
        update_file_if_value(x264_linux, "Docker x264 URL", dockerfile, r"wget https://github.com/rigaya/AutoBuildForAviUtlPlugins/releases/download/[^/]+/[^/]+ -O x264.tar.xz", f"wget {x264_linux} -O x264.tar.xz")
        update_file_if_value(x265_linux, "Docker x265 URL", dockerfile, r"wget https://github.com/rigaya/AutoBuildForAviUtlPlugins/releases/download/[^/]+/[^/]+ -O x265.tar.xz", f"wget {x265_linux} -O x265.tar.xz")
        update_file_if_value(svt_linux, "Docker SVT-AV1 URL", dockerfile, r"wget https://github.com/rigaya/AutoBuildForAviUtlPlugins/releases/download/[^/]+/[^/]+ -O svt-av1.tar.xz", f"wget {svt_linux} -O svt-av1.tar.xz")

        # 2. Docker encoders
        encoder_repos = {
            "QSVENCC_VER": "rigaya/QSVEnc",
            "NVENCC_VER": "rigaya/NVEnc",
            "VCEENCC_VER": "rigaya/VCEEnc",
            "TSREPLACE_VER": "rigaya/tsreplace"
        }
        
        for env_var, repo in encoder_repos.items():
            try:
                print(f"Fetching {repo} latest release...")
                release = get_latest_release(repo)
                ver = release['tag_name']

                # Update all occurrences of ENV var
                pattern = rf"ENV {env_var}=[^ \n]+"
                replacement = f"ENV {env_var}={ver}"
                update_file_if_value(ver, env_var, dockerfile, pattern, replacement)
            except Exception as e:
                print(f"Warning: Failed to update {env_var} from {repo}: {e}. Keeping existing value.")

        # 3. Verification
        print("Verifying changes...")
        verify_changes()

    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
