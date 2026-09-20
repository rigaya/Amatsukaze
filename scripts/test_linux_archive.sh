#!/usr/bin/env bash
set -euo pipefail

package_dir=
images=ubuntu:20.04,ubuntu:24.04,debian:12,debian:13,fedora:latest,archlinux:latest
builder_image=ghcr.io/rigaya/amatsukaze-builder:ubuntu2004
while (($#)); do
    case "$1" in
        --package) package_dir=$2; shift 2 ;;
        --images) images=$2; shift 2 ;;
        --builder-image) builder_image=$2; shift 2 ;;
        *) echo "使い方: $0 --package 展開先 [--images イメージ名,...] [--builder-image イメージ名]" >&2; exit 2 ;;
    esac
done
[[ -n "$package_dir" ]] || { echo '--package が必要です' >&2; exit 2; }
package_dir=$(realpath "$package_dir")
[[ -f "$package_dir/basepkg-manifest.json" && -x "$package_dir/AmatsukazeServer.sh" ]] || {
    echo "完成した配布ツリーを指定してください: $package_dir" >&2
    exit 2
}

test_dir=$(mktemp -d)
trap 'rm -r "$test_dir"' EXIT
cat > "$test_dir/avisynth_probe.c" <<'SOURCE'
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct AVS_ScriptEnvironment AVS_ScriptEnvironment;
typedef struct AVS_Value AVS_Value;
// 配布版AviSynth 3.7.5のC APIに合わせた値の配置。
struct AVS_Value {
    short type;
    short array_size;
    union {
        void *ptr;
        int integer;
        const char *string;
        int64_t longlong;
        double number;
    } d;
};

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--open") == 0) {
        void *plugin = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
        if (!plugin) {
            fprintf(stderr, "フィルタを読み込めません: %s: %s\n", argv[2], dlerror());
            return 1;
        }
        dlclose(plugin);
        printf("共有ライブラリ読み込み成功: %s\n", argv[2]);
        return 0;
    }
    if (argc != 2) return 2;
    void *library = dlopen("/source/exe_files/lib/libavisynth.so.11", RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        fprintf(stderr, "AviSynthを読み込めません: %s\n", dlerror());
        return 1;
    }
    AVS_ScriptEnvironment *(*create)(int) = dlsym(library, "avs_create_script_environment");
    AVS_Value (*invoke)(AVS_ScriptEnvironment *, const char *, AVS_Value, const char **) = dlsym(library, "avs_invoke");
    void (*release)(AVS_Value) = dlsym(library, "avs_release_value");
    void (*destroy)(AVS_ScriptEnvironment *) = dlsym(library, "avs_delete_script_environment");
    if (!create || !invoke || !release || !destroy) {
        fputs("AviSynthのC APIが不足しています\n", stderr);
        return 1;
    }
    AVS_ScriptEnvironment *env = create(11);
    if (!env) {
        fputs("AviSynth環境を作成できません\n", stderr);
        return 1;
    }
    AVS_Value arg = { .type = 's', .d.string = argv[1] };
    AVS_Value result = invoke(env, "LoadPlugin", arg, NULL);
    if (result.type == 'e') {
        fprintf(stderr, "LoadPluginに失敗しました: %s: %s\n", argv[1], result.d.string);
        release(result);
        destroy(env);
        return 1;
    }
    release(result);
    destroy(env);
    printf("LoadPlugin成功: %s\n", argv[1]);
    return 0;
}
SOURCE

docker run --rm -v "$test_dir:/test" "$builder_image" \
    cc -O2 -Wall -Wextra -o /test/avisynth_probe /test/avisynth_probe.c -ldl

cat > "$test_dir/container_test.sh" <<'TEST'
set -euo pipefail
export OPENSSL_MODULES=/source/exe_files/lib/ossl-modules
export DOTNET_SYSTEM_GLOBALIZATION_APPLOCALICU=76.1
cd /source

elf_count=0
while IFS= read -r -d '' path; do
    [[ $(od -An -N4 -tx1 "$path" | tr -d ' \n') == 7f454c46 ]] || continue
    elf_count=$((elf_count + 1))
    result=$(ldd "$path" 2>&1) && status=0 || status=$?
    if [[ "$result" == *'not found'* ]]; then
        echo "共有ライブラリが不足しています: ${path#/source/}" >&2
        echo "$result" >&2
        exit 1
    fi
    if (( status != 0 )) && [[ "$result" != *'not a dynamic executable'* && "$result" != *'statically linked'* ]]; then
        echo "lddに失敗しました: ${path#/source/}" >&2
        echo "$result" >&2
        exit 1
    fi
    while IFS= read -r line; do
        case "$line" in
            *'libstdc++.so.6 => '*|*'libgcc_s.so.1 => '*|*'libavisynth.so.11 => '*|\
            *'libssl.so.3 => '*|*'libcrypto.so.3 => '*|*'libicuuc.so.76 => '*|\
            *'libicui18n.so.76 => '*|*'libicudata.so.76 => '*)
                library_path=${line#*' => '}
                library_path=${library_path%% *}
                if [[ $(realpath -m "$library_path") != /source/exe_files/lib/* ]]; then
                    echo "同梱ライブラリを参照していません: ${path#/source/}: $line" >&2
                    exit 1
                fi ;;
        esac
    done <<< "$result"
done < <(find /source/exe_files -type f -print0)
echo "ELF依存検査成功: $elf_count 件"
(( elf_count > 0 )) || exit 1

cli_count=0
while IFS= read -r -d '' path; do
    [[ $(od -An -N4 -tx1 "$path" | tr -d ' \n') == 7f454c46 ]] || continue
    case "$path" in
        */lib/*|*/plugins64/*|*.so|*.so.*|*/AmatsukazeServerCLI) continue ;;
    esac
    cli_count=$((cli_count + 1))
    result=$(timeout 10s "$path" --help </dev/null 2>&1) && status=0 || status=$?
    actual_status=$status
    if (( status == 255 )) && [[ "$result" == *'usage:'* ]]; then status=0; fi
    if (( status == 124 || status == 125 || status == 126 || status == 127 \
        || (status >= 128 && status != 255) )) \
        || (( status == 255 )) \
        || [[ "$result" == *'error while loading shared libraries'* \
        || "$result" == *"Couldn't find a valid ICU package"* \
        || "$result" == *'No usable version of libssl'* ]]; then
        echo "起動に失敗しました: ${path#/source/} (終了値: $status)" >&2
        echo "$result" >&2
        exit 1
    fi
    echo "起動確認: ${path#/source/} (終了値: $actual_status)"
done < <(find /source/exe_files -maxdepth 2 -type f -perm /111 -print0)
echo "CLI起動検査成功: $cli_count 件"
(( cli_count > 0 )) || exit 1

shell_count=0
for path in /source/exe_files/cmd/*; do
    [[ -f "$path" && -x "$path" ]] || continue
    result=$(timeout 10s "$path" --help </dev/null 2>&1) && status=0 || status=$?
    if (( status != 0 )) || [[ "$result" != --help ]]; then
        echo "コマンド用スクリプトを起動できません: ${path#/source/} (終了値: $status)" >&2
        echo "$result" >&2
        exit 1
    fi
    echo "起動確認: ${path#/source/}"
    shell_count=$((shell_count + 1))
done
echo "コマンド用スクリプト起動成功: $shell_count 件"
(( shell_count > 0 )) || exit 1

plugin_count=0
gpu_count=0
plugin_failed=0
while IFS= read -r -d '' plugin; do
    case "${plugin##*/}" in
        AvsCUDA.so|KFM.so|KNNEDI3.so|KTGMC.so|KUtil.so)
            if ! timeout 10s /test/avisynth_probe --open "$plugin" </dev/null; then
                plugin_failed=$((plugin_failed + 1))
                continue
            fi
            gpu_count=$((gpu_count + 1))
            continue ;;
    esac
    if ! timeout 10s /test/avisynth_probe "$plugin" </dev/null; then
        plugin_failed=$((plugin_failed + 1))
        continue
    fi
    plugin_count=$((plugin_count + 1))
done < <(find /source/exe_files/plugins64 -maxdepth 1 -type f -name '*.so' -print0)
echo "AviSynthフィルタ登録成功: $plugin_count 件、CUDA用読み込み成功: $gpu_count 件、失敗: $plugin_failed 件"
(( plugin_count > 0 )) || exit 1
(( plugin_failed == 0 )) || exit 1

mkdir -p /tmp/amatsukaze-package
cp -a /source/. /tmp/amatsukaze-package/
cd /tmp/amatsukaze-package
timeout 20s ./AmatsukazeServer.sh > /tmp/amatsukaze-server.log 2>&1 &
server_pid=$!
ready=false
for i in {1..15}; do
    if (echo > /dev/tcp/127.0.0.1/32768) 2>/dev/null \
        && (echo > /dev/tcp/127.0.0.1/32769) 2>/dev/null; then
        ready=true
        break
    fi
    if ! kill -0 "$server_pid" 2>/dev/null; then break; fi
    sleep 1
done
if [[ "$ready" == true ]]; then
    sleep 2
    kill -0 "$server_pid" 2>/dev/null || ready=false
fi
kill "$server_pid" 2>/dev/null || true
wait "$server_pid" 2>/dev/null || true
if [[ "$ready" != true ]]; then
    cat /tmp/amatsukaze-server.log >&2
    echo "サーバー起動に失敗しました: $IMAGE" >&2
    exit 1
fi
echo "サーバー起動成功: $IMAGE"
TEST
IFS=, read -r -a image_list <<< "$images"
(( ${#image_list[@]} > 0 )) || { echo '検査対象のOSがありません' >&2; exit 2; }
for image in "${image_list[@]}"; do
    [[ -n "$image" ]] || { echo '空のイメージ名があります' >&2; exit 2; }
    echo "検査開始: $image"
    docker run --rm -e IMAGE="$image" \
        -v "$package_dir:/source:ro" -v "$test_dir:/test:ro" \
        "$image" bash /test/container_test.sh
done
echo 'すべてのOSで検査に成功しました。'
