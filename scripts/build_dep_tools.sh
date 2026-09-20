#!/usr/bin/env bash
set -euo pipefail

work_dir=../build_tmp/dep_tools
output_dir=../artifacts/basepkg
only=
version=local
assets_only=false
package_dir=
while (($#)); do
    case "$1" in
        --work-dir) work_dir=$2; shift 2 ;;
        --output-dir) output_dir=$2; shift 2 ;;
        --version) version=$2; shift 2 ;;
        --only) only=${only:+$only,}$2; shift 2 ;;
        --assets-only) assets_only=true; shift ;;
        --package-dir) package_dir=$2; shift 2 ;;
        *) echo "不明な引数: $1" >&2; exit 2 ;;
    esac
done

if [[ "$assets_only" == true ]]; then
    [[ -n "$package_dir" && -d "$package_dir/exe_files" ]] || { echo "既存のパッケージを --package-dir で指定してください" >&2; exit 2; }
elif [[ -n "$package_dir" ]]; then
    echo "--package-dir は --assets-only と一緒に指定してください" >&2
    exit 2
fi

[[ "$version" =~ ^[A-Za-z0-9._-]+$ ]] || { echo "版に使用できない文字があります: $version" >&2; exit 2; }
if [[ -n "$only" ]]; then
    IFS=, read -r -a requested <<< "$only"
    for name in "${requested[@]}"; do
        case "$name" in
            avisynth|avisynthcudafilters|x264|x265|svt_av1|x262|tsreplace|yadifmod2|tivtc|nnedi3|masktools|mvtools|rgtools|mp4box|lsmash|chapter_exe|join_logo_scp|tsreadex|psisiarc|b24tovtt|opusenc|mkvmerge|icu|openssl|SCRenamePy) ;;
            *) echo "不明な対象: $name" >&2; exit 2 ;;
        esac
        if [[ "$assets_only" == true ]]; then
            case "$name" in
                avisynth|avisynthcudafilters|x264|x265|svt_av1|x262|tsreplace|SCRenamePy) ;;
                *) echo "配布アセットではない対象: $name" >&2; exit 2 ;;
            esac
        else
            case "$name" in
                avisynthcudafilters|x264|x265|svt_av1|x262|tsreplace|SCRenamePy)
                    echo "既存アセットは --assets-only で最終パッケージへ配置してください: $name" >&2
                    exit 2 ;;
            esac
        fi
    done
fi

work_dir=$(realpath -m "$work_dir")
output_dir=$(realpath -m "$output_dir")
mkdir -p "$work_dir/downloads" "$work_dir/sources" "$work_dir/prefix" "$output_dir/logs"
log=$output_dir/logs/build.log
if [[ "$assets_only" == true ]]; then
    stage=$(realpath "$package_dir")
else
    stage=$(mktemp -d "$work_dir/stage.XXXXXXXX")
fi
mkdir -p "$stage/exe_files" "$stage/exe_files/lib" "$stage/exe_files/plugins64"
prefix=$work_dir/prefix
sources=$work_dir/sources
builds=$work_dir/builds
mkdir -p "$builds"

want() {
    [[ -z "$only" || ",$only," == *",$1,"* ]]
}

run_in() {
    local directory=$1
    shift
    echo "実行: $*" >&2
    if ! (cd "$directory" && "$@") >> "$log" 2>&1; then
        echo "ビルドに失敗しました。ログ: $log" >&2
        return 1
    fi
}

download() {
    local name=$1 url=$2 archive=$work_dir/downloads/$1
    local directory=${name%.tar.gz}
    directory=${directory%.tar.xz}
    if [[ ! -s "$archive" ]]; then
        echo "取得: $name" >&2
        curl --fail --location --retry 3 --silent --show-error --output "$archive.download" "$url"
        mv "$archive.download" "$archive"
    fi
    if [[ ! -d "$work_dir/sources/$directory" ]]; then
        tar -xf "$archive" -C "$work_dir/sources"
    fi
}

fetch() {
    local name=$1 url=$2 release archive
    release=${url%/*}
    release=${release##*/}
    archive=$work_dir/downloads/${name}_${release}_${url##*/}
    if [[ ! -s "$archive" ]]; then
        echo "取得: $name" >&2
        curl --fail --location --retry 3 --silent --show-error --output "$archive.download" "$url"
        mv "$archive.download" "$archive"
    fi
    printf '%s\n' "$archive"
}

checkout() {
    local name=$1 url=$2 commit=$3 path=$sources/$1
    if [[ ! -d "$path/.git" ]]; then
        run_in "$sources" git clone "$url" "$path"
    fi
    if [[ "$(git -C "$path" rev-parse HEAD)" != "$commit" ]]; then
        run_in "$path" git fetch origin "$commit"
        run_in "$path" git checkout --detach "$commit"
    fi
    printf '%s\n' "$path"
}

copy_asset() {
    local name=$1 url=$2 archive temp
    archive=$(fetch "$name" "$url")
    temp=$(mktemp -d "$work_dir/asset.XXXXXXXX")
    if [[ "$name" == tsreplace ]]; then
        dpkg-deb -x "$archive" "$temp"
        cp "$temp/usr/bin/tsreplace" "$stage/exe_files/tsreplace"
    else
        tar -xf "$archive" -C "$temp"
        case "$name" in
            avisynth)
                if [[ "$assets_only" == true ]]; then
                    cp -a "$temp/usr/local/lib/libavisynth.so"* "$stage/exe_files/lib/"
                    cp -a "$temp/usr/local/lib/avisynth/"*.so "$stage/exe_files/plugins64/"
                else
                    mkdir -p "$prefix/include" "$prefix/lib/pkgconfig"
                    cp -a "$temp/usr/local/include/avisynth" "$prefix/include/"
                    cp -a "$temp/usr/local/lib/libavisynth.so"* "$prefix/lib/"
                    cp -a "$temp/usr/local/lib/pkgconfig/avisynth.pc" "$prefix/lib/pkgconfig/"
                    sed -i "s|/usr/local|$prefix|g" "$prefix/lib/pkgconfig/avisynth.pc"
                fi
                ;;
            avisynthcudafilters)
                cp -a "$temp/usr/local/lib/avisynth/"*.so "$stage/exe_files/plugins64/"
                ;;
            x264|x265|x262|svt_av1)
                local binary=$name
                [[ "$name" == svt_av1 ]] && binary=SvtAv1EncApp
                cp "$temp/$binary" "$stage/exe_files/$binary"
                ;;
        esac
    fi
    rm -r "$temp"
}

build_source() {
    local name=$1 url=$2 commit=$3 method=$4 pattern=$5 path copy build result
    path=$(checkout "$name" "$url" "$commit")
    build=$builds/$name
    mkdir -p "$build"
    export CPATH="$prefix/include/avisynth:$prefix/include"
    export LIBRARY_PATH="$prefix/lib"
    export PKG_CONFIG_PATH="$prefix/lib/pkgconfig"
    export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig"
    export CMAKE_PREFIX_PATH="$prefix"
    export CFLAGS='-O2 -fPIC -march=x86-64'
    export CXXFLAGS='-O2 -fPIC -march=x86-64'
    export LDFLAGS=
    [[ "$name" == tivtc ]] && CXXFLAGS+=' -include cstdint'
    [[ "$name" == nnedi3 ]] && LDFLAGS=-pthread
    case "$method" in
        cmake*)
            local sub=${method#cmake:}
            [[ "$sub" == "$method" ]] && sub=.
            if [[ ! -f "$build/CMakeCache.txt" ]]; then
                run_in "$path" cmake -S "$path/$sub" -B "$build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_PREFIX_PATH="$prefix"
            fi
            run_in "$path" cmake --build "$build" --parallel "$(nproc)"
            result=$build
            ;;
        meson)
            if [[ ! -f "$build/build.ninja" ]]; then
                run_in "$path" meson setup "$build" "$path" --buildtype=release --prefix="$prefix"
            fi
            run_in "$path" ninja -C "$build"
            result=$build
            ;;
        make*)
            copy=$build/source
            if [[ ! -d "$copy" ]]; then
                mkdir -p "$copy"
                cp -a "$path/." "$copy/"
            fi
            local sub=${method#make:}
            [[ "$sub" == "$method" || "$sub" == gpac || "$sub" == lsmash ]] && sub=.
            case "$name" in
                mp4box|lsmash|tsreadex|psisiarc|b24tovtt) LDFLAGS=-static ;;
            esac
            case "$name" in
                mp4box) run_in "$copy" ./configure --static-bin --disable-shared --disable-ssl --disable-x11 ;;
                lsmash) run_in "$copy" ./configure --extra-ldflags=-static ;;
            esac
            run_in "$copy/$sub" make -j "$(nproc)"
            result=$copy
            ;;
    esac
    local found
    found=$(find "$result" -type f -name "$pattern" -print -quit)
    [[ -n "$found" ]] || { echo "生成物がありません: $name / $pattern" >&2; exit 1; }
    if [[ "$name" == yadifmod2 || "$name" == tivtc || "$name" == nnedi3 || "$name" == masktools || "$name" == mvtools || "$name" == rgtools ]]; then
        cp "$found" "$stage/exe_files/plugins64/"
    else
        cp "$found" "$stage/exe_files/"
    fi
    if [[ "$name" == tivtc ]]; then
        cp "$(find "$build" -type f -name libtdeint.so -print -quit)" "$stage/exe_files/plugins64/"
    elif [[ "$name" == lsmash ]]; then
        cp "$(find "$result" -type f -name timelineeditor -print -quit)" "$stage/exe_files/"
    fi
    case "$name" in
        mp4box|lsmash|tsreadex|psisiarc|b24tovtt) assert_static "$stage/exe_files/$(basename "$found")" ;;
    esac
}

build_mkvmerge() {
    download zlib-1.3.1.tar.gz https://zlib.net/fossils/zlib-1.3.1.tar.gz
    download libogg-1.3.5.tar.xz https://downloads.xiph.org/releases/ogg/libogg-1.3.5.tar.xz
    download libvorbis-1.3.7.tar.xz https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.xz
    download boost_1_71_0.tar.gz https://archives.boost.io/release/1.71.0/source/boost_1_71_0.tar.gz
    download mkvtoolnix-45.0.0.tar.xz https://mkvtoolnix.download/sources/mkvtoolnix-45.0.0.tar.xz

    export PKG_CONFIG_LIBDIR=$prefix/lib/pkgconfig
    export PKG_CONFIG_PATH=$PKG_CONFIG_LIBDIR
    export CPPFLAGS=-I$prefix/include
    export LDFLAGS=-L$prefix/lib
    export CFLAGS='-O2 -march=x86-64'
    export CXXFLAGS='-O2 -march=x86-64'

    if [[ ! -f "$prefix/lib/libz.a" ]]; then
        run_in "$sources/zlib-1.3.1" ./configure --static --prefix="$prefix"
        run_in "$sources/zlib-1.3.1" make -j4
        run_in "$sources/zlib-1.3.1" make install
    fi
    if [[ ! -f "$prefix/lib/libogg.a" ]]; then
        run_in "$sources/libogg-1.3.5" ./configure --prefix="$prefix" --disable-shared --enable-static
        run_in "$sources/libogg-1.3.5" make -j4
        run_in "$sources/libogg-1.3.5" make install
    fi
    if [[ ! -f "$prefix/lib/libvorbis.a" ]]; then
        run_in "$sources/libvorbis-1.3.7" ./configure --prefix="$prefix" --disable-shared --enable-static
        run_in "$sources/libvorbis-1.3.7" make -j4
        run_in "$sources/libvorbis-1.3.7" make install
    fi
    if [[ ! -f "$prefix/lib/libboost_filesystem.a" ]]; then
        run_in "$sources/boost_1_71_0" ./bootstrap.sh --prefix="$prefix" --with-libraries=filesystem,system,date_time
        run_in "$sources/boost_1_71_0" ./b2 -j4 --with-filesystem --with-system --with-date_time link=static threading=multi variant=release --prefix="$prefix" install
    fi
    if [[ ! -f "$sources/mkvtoolnix-45.0.0/src/mkvmerge" ]]; then
        run_in "$sources/mkvtoolnix-45.0.0" ./configure --enable-static --disable-qt --disable-magic --without-flac --without-gettext --with-boost="$prefix" --with-boost-libdir="$prefix/lib" --with-extra-includes="$prefix/include" --with-extra-libs="$prefix/lib"
        run_in "$sources/mkvtoolnix-45.0.0" rake -j4 src/mkvmerge
    fi
    local binary=$sources/mkvtoolnix-45.0.0/src/mkvmerge
    if readelf -d "$binary" | grep -q '(NEEDED)' || readelf -l "$binary" | grep -q INTERP; then
        echo "mkvmerge に共有依存が残っています" >&2
        exit 1
    fi
    cp "$binary" "$stage/exe_files/mkvmerge"
}

build_opusenc() {
    local path
    export PKG_CONFIG_LIBDIR=$prefix/lib/pkgconfig
    export PKG_CONFIG_PATH=$PKG_CONFIG_LIBDIR
    export CPPFLAGS=-I$prefix/include
    export CFLAGS='-O2 -march=x86-64'
    export LDFLAGS=-L$prefix/lib
    if [[ ! -f "$prefix/lib/libogg.a" ]]; then
        download libogg-1.3.5.tar.xz https://downloads.xiph.org/releases/ogg/libogg-1.3.5.tar.xz
        run_in "$sources/libogg-1.3.5" ./configure --prefix="$prefix" --disable-shared --enable-static
        run_in "$sources/libogg-1.3.5" make -j "$(nproc)" install
    fi
    path=$(checkout opus https://github.com/xiph/opus.git ddbe48383984d56acd9e1ab6a090c54ca6b735a6)
    if [[ ! -f "$prefix/lib/libopus.a" ]]; then
        run_in "$path" cmake -S "$path" -B "$builds/opus" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DBUILD_SHARED_LIBS=OFF -DOPUS_BUILD_SHARED_LIBRARY=OFF -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF
        run_in "$path" cmake --build "$builds/opus" --parallel "$(nproc)"
        run_in "$path" cmake --install "$builds/opus"
    fi
    path=$(checkout libopusenc https://github.com/xiph/libopusenc.git b19e1b14dee3e5245f2e37bfb193bde72fa70a2d)
    if [[ ! -f "$prefix/lib/libopusenc.a" ]]; then
        cp -a "$path/." "$builds/libopusenc/"
        printf 'PACKAGE_VERSION="0.2.1"\n' > "$builds/libopusenc/package_version"
        run_in "$builds/libopusenc" ./autogen.sh
        run_in "$builds/libopusenc" ./configure --prefix="$prefix" --disable-shared --enable-static --disable-examples
        run_in "$builds/libopusenc" make -j "$(nproc)" install
    fi
    path=$(checkout opus-tools https://github.com/xiph/opus-tools.git 0c1337f57e5b87fd23421904303fed3e575ce354)
    if [[ ! -f "$builds/opus-tools/opusenc" ]]; then
        cp -a "$path/." "$builds/opus-tools/"
        printf 'PACKAGE_VERSION="0.2"\n' > "$builds/opus-tools/package_version"
        run_in "$builds/opus-tools" ./autogen.sh
        OPUSFILE_CFLAGS=' ' OPUSFILE_LIBS=' ' OPUSURL_CFLAGS=' ' OPUSURL_LIBS=' ' run_in "$builds/opus-tools" ./configure --prefix="$prefix" --disable-shared --enable-static --without-flac
        run_in "$builds/opus-tools" make -j "$(nproc)" opusenc LDFLAGS=-all-static
    fi
    cp "$builds/opus-tools/opusenc" "$stage/exe_files/opusenc"
    assert_static "$stage/exe_files/opusenc"
}

build_icu() {
    local archive
    archive=$(fetch icu4c-76_1-src.tgz https://github.com/unicode-org/icu/releases/download/release-76-1/icu4c-76_1-src.tgz)
    if [[ ! -f "$sources/icu/source/configure" ]]; then
        tar -xf "$archive" -C "$sources"
    fi
    mkdir -p "$builds/icu"
    if [[ ! -f "$builds/icu/Makefile" ]]; then
        run_in "$builds/icu" "$sources/icu/source/configure" --prefix="$prefix" --libdir="$prefix/lib" --disable-tests --disable-samples --disable-extras
    fi
    run_in "$builds/icu" make -j "$(nproc)"
    run_in "$builds/icu" make install
    cp -a "$prefix/lib/"libicuuc.so* "$prefix/lib/"libicui18n.so* "$prefix/lib/"libicudata.so* "$stage/exe_files/lib/"
}

build_openssl() {
    download openssl-3.5.8.tar.gz https://github.com/openssl/openssl/releases/download/openssl-3.5.8/openssl-3.5.8.tar.gz
    if [[ ! -f "$sources/openssl-3.5.8/Makefile" ]]; then
        run_in "$sources/openssl-3.5.8" ./Configure linux-x86_64 shared no-tests no-apps no-docs --prefix="$prefix" --libdir=lib
    fi
    run_in "$sources/openssl-3.5.8" make -j "$(nproc)"
    run_in "$sources/openssl-3.5.8" make install_sw
    cp -a "$prefix/lib/"libssl.so* "$prefix/lib/"libcrypto.so* "$stage/exe_files/lib/"
    if [[ -d "$prefix/lib/ossl-modules" ]]; then
        mkdir -p "$stage/exe_files/lib/ossl-modules"
        cp -a "$prefix/lib/ossl-modules/"*.so "$stage/exe_files/lib/ossl-modules/"
    fi
}

assert_static() {
    if readelf -d "$1" | grep -q '(NEEDED)' || readelf -l "$1" | grep -q INTERP; then
        echo "共有依存が残っています: $1" >&2
        exit 1
    fi
}

build_screname() {
    local archive temp
    archive=$(fetch SCRenamePy-0.05.tar.gz 'https://github.com/rigaya/SCRenamePy/archive/refs/tags/0.05.tar.gz')
    temp=$(mktemp -d "$work_dir/screname.XXXXXXXX")
    tar -xf "$archive" -C "$temp"
    mkdir -p "$stage/exe_files/SCRenamePy"
    cp "$temp/SCRenamePy-0.05/"SCRename.* "$stage/exe_files/SCRenamePy/"
    chmod +x "$stage/exe_files/SCRenamePy/SCRename.py"
    rm -r "$temp"
}

if [[ "$assets_only" == true ]] && ! command -v patchelf >/dev/null; then
    echo '配布アセットの RUNPATH 設定には patchelf が必要です' >&2
    exit 1
fi

if [[ "$assets_only" != true && -n "$only" && ",$only," != *",avisynth,"* ]]; then
    for dependent in yadifmod2 tivtc nnedi3 masktools mvtools rgtools chapter_exe join_logo_scp; do
        if want "$dependent"; then only=avisynth,$only; break; fi
    done
fi

for name in avisynth avisynthcudafilters x264 x265 svt_av1 x262 tsreplace; do
    if [[ "$assets_only" != true && "$name" != avisynth ]]; then continue; fi
    want "$name" || continue
    case "$name" in
        avisynth) url=https://github.com/rigaya/AviSynthCUDAFilters/releases/download/0.7.5/avisynth_3.7.5-1_amd64_linux.tar.xz ;;
        avisynthcudafilters) url=https://github.com/rigaya/AviSynthCUDAFilters/releases/download/0.7.5/avisynthcudafilters_0.7.5-1_amd64_linux.tar.xz ;;
        x264) url=https://github.com/rigaya/AutoBuildForAviUtlPlugins/releases/download/auto-build-20260919-084954/x264_3223_amd64_linux.tar.xz ;;
        x265) url=https://github.com/rigaya/AutoBuildForAviUtlPlugins/releases/download/auto-build-20260919-084954/x265_4.3%2B45_amd64_linux.tar.xz ;;
        svt_av1) url=https://github.com/rigaya/AutoBuildForAviUtlPlugins/releases/download/auto-build-20260919-084954/SvtAv1EncApp_4.2.0-146_amd64_linux_clang.tar.xz ;;
        x262) url=https://github.com/rigaya/AutoBuildForAviUtlPlugins/releases/download/auto-build-20250907-045307/x262_20260901.tar.gz ;;
        tsreplace) url=https://github.com/rigaya/tsreplace/releases/download/0.20/tsreplace_0.20_amd64.deb ;;
    esac
    copy_asset "$name" "$url"
done

if [[ "$assets_only" == true ]]; then
    if want SCRenamePy; then build_screname; fi
    for path in "$stage/exe_files/cmd/"*; do
        [[ -f "$path" ]] || continue
        sed -i 's/\r$//' "$path"
    done
    if [[ -f "$stage/exe_files/cmd/SetPriority" ]]; then
        sed -i 's|^exec .*SetPriority.*$|exec "$(dirname "$0")/../ScriptCommand" SetPriority "$@"|' "$stage/exe_files/cmd/SetPriority"
    fi
    if [[ -f "$stage/exe_files/lib/libicuuc.so.76.1" && -f "$stage/exe_files/lib/libcrypto.so.3" ]]; then
        cat > "$stage/AmatsukazeServer.sh" <<'LAUNCHER'
#!/bin/sh
cd "$(dirname "$0")" || exit 1
export PATH="$PWD/exe_files:$PATH"
export OPENSSL_MODULES="$PWD/exe_files/lib/ossl-modules"
export DOTNET_SYSTEM_GLOBALIZATION_APPLOCALICU=76.1
exec ./exe_files/AmatsukazeServerCLI -p 32768
LAUNCHER
        chmod +x "$stage/AmatsukazeServer.sh"
    fi
    while IFS= read -r -d '' path; do
        readelf -d "$path" 2>/dev/null | grep -q '(NEEDED)' || continue
        case "$path" in
            "$stage/exe_files/lib/ossl-modules/"*) rpath='$ORIGIN/..' ;;
            "$stage/exe_files/lib/"*) rpath='$ORIGIN' ;;
            "$stage/exe_files/plugins64/"*|"$stage/exe_files/7z/"*) rpath='$ORIGIN/../lib' ;;
            *) rpath='$ORIGIN/lib' ;;
        esac
        patchelf --set-rpath "$rpath" "$path"
    done < <(find "$stage/exe_files" -type f -print0)
    echo "配布アセットを配置しました: $stage"
    exit 0
fi

while read -r name url commit method pattern; do
    want "$name" || continue
    if [[ "$name" == yadifmod2 || "$name" == tivtc || "$name" == nnedi3 || "$name" == masktools || "$name" == mvtools || "$name" == rgtools || "$name" == chapter_exe || "$name" == join_logo_scp ]]; then
        [[ -f "$prefix/lib/libavisynth.so" ]] || { echo "AviSynth が必要です: $name" >&2; exit 1; }
    fi
    build_source "$name" "$url" "$commit" "$method" "$pattern"
done <<'SOURCES'
yadifmod2 https://github.com/Asd-g/yadifmod2.git 9db5d2118dc2800701c5137afcfe45f3163211da cmake libyadifmod2*.so
tivtc https://github.com/pinterf/TIVTC.git 5cfe78777a0665f33a8109502371bcb3b652be41 cmake:src libtivtc.so
nnedi3 https://github.com/rigaya/NNEDI3.git a09c8e743ac42c7552bdf2514605e7e0253da0b6 meson libnnedi3.so
masktools https://github.com/pinterf/masktools.git 8291927bf6956981a6412d353da8ca39d49c9d3a cmake libmasktools2.so
mvtools https://github.com/pinterf/mvtools.git a488b095c4bdc8d81abfd952ab534c015a9d45b7 cmake libmvtools2.so
rgtools https://github.com/pinterf/RgTools.git a9cff29cb228b8a6fb52148f334554f4c3823798 cmake librgtools.so
mp4box https://github.com/gpac/gpac.git 5d70253ac94e5840be7b86054131dd753af63cc7 make:gpac MP4Box
lsmash https://github.com/l-smash/l-smash.git 18a9ed25c7ff79a7f4f4bf850c345c72179b8998 make:lsmash muxer
chapter_exe https://github.com/rigaya/chapter_exe.git 32880d45f088e574285a101e6a49b032bb04f6ea make:src chapter_exe
join_logo_scp https://github.com/yobibi/join_logo_scp.git 4107b3e0e1a798287603b76b8d6d734c9c01396a make:src join_logo_scp
tsreadex https://github.com/xtne6f/tsreadex.git eddc8bca0de99627d3867259e7a6e777cbd3b3c6 make tsreadex
psisiarc https://github.com/xtne6f/psisiarc.git 6593a0f63aedaaecfac7682b51e267874a8ec549 make psisiarc
b24tovtt https://github.com/xtne6f/b24tovtt.git 4437fdde8e8b6693e2b65ff9808211f6c7dc673a make b24tovtt
SOURCES

if want opusenc; then build_opusenc; fi
if want mkvmerge; then build_mkvmerge; assert_static "$stage/exe_files/mkvmerge"; fi
if want icu; then build_icu; fi
if want openssl; then build_openssl; fi

if want avisynth; then
    cp "$(c++ -print-file-name=libstdc++.so.6)" "$stage/exe_files/lib/"
    cp "$(cc -print-file-name=libgcc_s.so.1)" "$stage/exe_files/lib/"
fi
if command -v patchelf >/dev/null; then
    while IFS= read -r -d '' path; do
        readelf -d "$path" 2>/dev/null | grep -q '(NEEDED)' || continue
        case "$path" in
            "$stage/exe_files/plugins64/"*) patchelf --set-rpath '$ORIGIN/../lib' "$path" ;;
            "$stage/exe_files/lib/"*) patchelf --set-rpath '$ORIGIN' "$path" ;;
            *) patchelf --set-rpath '$ORIGIN/lib' "$path" ;;
        esac
    done < <(find "$stage/exe_files" -type f -print0)
fi

if [[ -z "$only" ]]; then
    printf '{"version":"%s","complete":true}\n' "$version" > "$stage/basepkg-manifest.json"
    archive=$output_dir/Amatsukaze_Linux_BasePkg_${version}_x64.tar.xz
    [[ ! -e "$archive" ]] || { echo "既存アーカイブがあります: $archive" >&2; exit 1; }
    tar -C "$stage" -cJf "$archive" .
    echo "BasePkg を作成しました: $archive"
else
    echo "部分ビルドを作成しました: $stage"
fi
