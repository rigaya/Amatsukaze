#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
使い方: test_basepkg.sh --basepkg 展開先
        test_basepkg.sh --package 最終パッケージの展開先

展開済みツリーの必須ファイル、リンク先、ELF の共有ライブラリ依存を検査します。
EOF
}

if [[ $# -eq 1 && ( "$1" == -h || "$1" == --help ) ]]; then
    usage
    exit 0
fi
if [[ $# -ne 2 ]]; then
    usage >&2
    exit 2
fi

mode=$1
case "$mode" in
    --basepkg|--package) ;;
    *) usage >&2; exit 2 ;;
esac

root=$(realpath "$2")
if [[ ! -d "$root/exe_files" ]]; then
    echo "exe_files が見つかりません: $root" >&2
    exit 1
fi

for command_name in readelf ldd realpath readlink find; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        echo "検査用コマンドが見つかりません: $command_name" >&2
        exit 1
    fi
done

exe_dir=$root/exe_files
lib_dir=$exe_dir/lib
failed=0

require_file() {
    local relative=$1
    if [[ ! -f "$root/$relative" ]]; then
        echo "必須ファイルがありません: $relative" >&2
        failed=1
    fi
}

require_executable() {
    local relative=$1
    require_file "$relative"
    if [[ -f "$root/$relative" && ! -x "$root/$relative" ]]; then
        echo "実行権限がありません: $relative" >&2
        failed=1
    fi
}

require_file exe_files/lib/libstdc++.so.6
require_file exe_files/lib/libgcc_s.so.1
require_file basepkg-manifest.json

if [[ "$mode" == --basepkg ]]; then
    for relative in exe_files/x264 exe_files/x265 exe_files/SvtAv1EncApp \
        exe_files/x262 exe_files/tsreplace exe_files/SCRenamePy \
        exe_files/lib/libavisynth.so.11 exe_files/plugins64/KFM.so; do
        if [[ -e "$root/$relative" ]]; then
            echo "既存アセットが BasePkg に混入しています: $relative" >&2
            failed=1
        fi
    done
fi

complete=false
if [[ -f "$root/basepkg-manifest.json" ]] && grep -q '"complete":true' "$root/basepkg-manifest.json"; then
    complete=true
fi

if [[ "$mode" == --package && "$complete" != true ]]; then
    echo '完成版 BasePkg のマニフェストがありません。' >&2
    failed=1
fi

if [[ "$complete" == true ]]; then
    for library_name in libicuuc.so.76.1 libicui18n.so.76.1 libicudata.so.76.1 \
        libssl.so.3 libcrypto.so.3; do
        require_file "exe_files/lib/$library_name"
    done
    for tool_name in opusenc mkvmerge MP4Box muxer timelineeditor \
        chapter_exe join_logo_scp tsreadex psisiarc b24tovtt; do
        require_executable "exe_files/$tool_name"
    done
    for tool_name in opusenc mkvmerge \
        MP4Box muxer timelineeditor tsreadex psisiarc b24tovtt; do
        tool_path=$exe_dir/$tool_name
        [[ -f "$tool_path" ]] || continue
        if readelf -d "$tool_path" | grep -q '(NEEDED)' || readelf -l "$tool_path" | grep -q INTERP; then
            echo "完全静的リンクではありません: exe_files/$tool_name" >&2
            failed=1
        fi
    done
    for plugin_name in libyadifmod2.so libtivtc.so libtdeint.so libnnedi3.so \
        libmasktools2.so libmvtools2.so librgtools.so; do
        if ! find "$exe_dir/plugins64" -maxdepth 1 -name "${plugin_name%.so}*.so" -print -quit | grep -q .; then
            echo "必須 AviSynth フィルタがありません: $plugin_name" >&2
            failed=1
        fi
    done
fi

if [[ "$mode" == --package ]]; then
    for tool_name in x264 x265 SvtAv1EncApp x262 tsreplace; do
        require_executable "exe_files/$tool_name"
    done
    for tool_name in x264 x265 SvtAv1EncApp x262; do
        tool_path=$exe_dir/$tool_name
        [[ -f "$tool_path" ]] || continue
        if readelf -d "$tool_path" | grep -q '(NEEDED)' || readelf -l "$tool_path" | grep -q INTERP; then
            echo "完全静的リンクではありません: exe_files/$tool_name" >&2
            failed=1
        fi
    done
    require_file exe_files/lib/libavisynth.so.11
    require_file exe_files/plugins64/KFM.so
    require_executable exe_files/SCRenamePy/SCRename.py
    require_executable AmatsukazeServer.sh
    require_executable exe_files/AmatsukazeServerCLI
    require_executable exe_files/AmatsukazeCLI
    require_file exe_files/libAmatsukaze.so
    require_file exe_files/libAmatsukaze2.so
    require_file exe_files/wwwroot/index.html

    for library in "$exe_dir/libAmatsukaze.so" "$exe_dir/libAmatsukaze2.so"; do
        if [[ -f "$library" ]] && ! readelf -d "$library" | grep -Fq '$ORIGIN/lib'; then
            echo "ライブラリの RUNPATH に \$ORIGIN/lib がありません: ${library#"$root"/}" >&2
            failed=1
        fi
    done
fi

# 絶対リンクと展開先の外へ出るリンクを拒否する。
while IFS= read -r -d '' link_path; do
    target=$(readlink "$link_path")
    if [[ "$target" = /* ]]; then
        echo "絶対リンクがあります: ${link_path#"$root"/} -> $target" >&2
        failed=1
        continue
    fi
    resolved=$(realpath -m "$(dirname "$link_path")/$target")
    if [[ "$resolved" != "$root" && "$resolved" != "$root/"* ]]; then
        echo "展開先の外を指すリンクがあります: ${link_path#"$root"/} -> $target" >&2
        failed=1
    elif [[ ! -e "$link_path" ]]; then
        echo "リンク先がありません: ${link_path#"$root"/} -> $target" >&2
        failed=1
    fi
done < <(find "$root" -type l -print0)

# ldd の「not found」を全 ELF で調べる。静的 ELF は依存検査の対象外。
elf_count=0
while IFS= read -r -d '' elf_path; do
    if [[ "$mode" == --basepkg && "$elf_path" == "$exe_dir/plugins64/"* ]]; then
        continue
    fi
    if ! readelf -h "$elf_path" >/dev/null 2>&1; then
        continue
    fi
    elf_count=$((elf_count + 1))
    dependency_result=$(LD_LIBRARY_PATH="$lib_dir:$exe_dir" ldd "$elf_path" 2>&1) || true
    if [[ "$dependency_result" == *'not found'* ]]; then
        echo "共有ライブラリを解決できません: ${elf_path#"$root"/}" >&2
        echo "$dependency_result" >&2
        failed=1
    fi
done < <(find "$exe_dir" -type f -print0)

if (( elf_count == 0 )); then
    echo 'ELF ファイルが見つかりません。' >&2
    failed=1
fi

if (( failed != 0 )); then
    exit 1
fi
echo "配布ツリーの検査に成功しました（ELF: $elf_count 件）。"
