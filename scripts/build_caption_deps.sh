#!/bin/sh
set -eu

# 字幕描画の依存を通常ビルドとprebuilt生成で共通化する。
# 引数はソース・ビルド作業先とbaselibsのインストール先。
CAPTION_BUILD_DIR=$(realpath -m "${1:?ビルド先が必要です}")
BASELIBS_DIR=$(realpath -m "${2:?インストール先が必要です}")
CAPTION_SOURCE_DIR="${CAPTION_BUILD_DIR}/caption_sources"
CAPTION_JOBS=$(nproc)
mkdir -p "${CAPTION_SOURCE_DIR}" "${BASELIBS_DIR}"
export PKG_CONFIG_PATH="${BASELIBS_DIR}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"

# 完了判定にはインストール済みの静的ライブラリとpcファイルを使う。
# 展開だけで中断したビルドも次回再開できる。
fetch_caption_source() {
    caption_name=$1
    caption_archive=$2
    caption_url=$3
    caption_sha256=$4
    if [ ! -f "${CAPTION_SOURCE_DIR}/${caption_archive}" ]; then
        wget "${caption_url}" -O "${CAPTION_SOURCE_DIR}/${caption_archive}"
    fi
    printf '%s  %s\n' "${caption_sha256}" "${CAPTION_SOURCE_DIR}/${caption_archive}" | sha256sum -c -
    if [ ! -d "${CAPTION_SOURCE_DIR}/${caption_name}" ]; then
        tar xf "${CAPTION_SOURCE_DIR}/${caption_archive}" -C "${CAPTION_SOURCE_DIR}"
    fi
}

if [ ! -f "${BASELIBS_DIR}/lib/libexpat.a" ] || [ ! -f "${BASELIBS_DIR}/lib/pkgconfig/expat.pc" ]; then
    echo "expatの静的ビルドを行います。"
    fetch_caption_source expat-2.8.5 expat-2.8.5.tar.xz \
        https://github.com/libexpat/libexpat/releases/download/R_2_8_5/expat-2.8.5.tar.xz \
        1e727b8933ec51a77a9a9d9afcf8e688bce45d907c13e36ab7393fe36e703182
    cmake -S "${CAPTION_SOURCE_DIR}/expat-2.8.5" -B "${CAPTION_SOURCE_DIR}/expat-2.8.5/_build" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_INSTALL_PREFIX="${BASELIBS_DIR}" -DCMAKE_INSTALL_LIBDIR=lib \
        -DEXPAT_SHARED_LIBS=OFF -DEXPAT_BUILD_TOOLS=OFF -DEXPAT_BUILD_EXAMPLES=OFF \
        -DEXPAT_BUILD_TESTS=OFF -DEXPAT_BUILD_DOCS=OFF -DEXPAT_BUILD_PKGCONFIG=ON
    cmake --build "${CAPTION_SOURCE_DIR}/expat-2.8.5/_build" --parallel "${CAPTION_JOBS}"
    cmake --install "${CAPTION_SOURCE_DIR}/expat-2.8.5/_build"
fi

if [ ! -f "${BASELIBS_DIR}/lib/libfreetype.a" ] || [ ! -f "${BASELIBS_DIR}/lib/pkgconfig/freetype2.pc" ]; then
    echo "FreeTypeの静的ビルドを行います。"
    fetch_caption_source freetype-2.13.3 freetype-2.13.3.tar.xz \
        https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz \
        0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289
    cmake -S "${CAPTION_SOURCE_DIR}/freetype-2.13.3" -B "${CAPTION_SOURCE_DIR}/freetype-2.13.3/_build" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_INSTALL_PREFIX="${BASELIBS_DIR}" -DCMAKE_INSTALL_LIBDIR=lib \
        -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_PNG=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_BROTLI=ON \
        -DFT_REQUIRE_ZLIB=ON -DZLIB_INCLUDE_DIR="${BASELIBS_DIR}/include" \
        -DZLIB_LIBRARY="${BASELIBS_DIR}/lib/libz.a"
    cmake --build "${CAPTION_SOURCE_DIR}/freetype-2.13.3/_build" --parallel "${CAPTION_JOBS}"
    cmake --install "${CAPTION_SOURCE_DIR}/freetype-2.13.3/_build"
fi

if [ ! -f "${BASELIBS_DIR}/lib/libfontconfig.a" ] || [ ! -f "${BASELIBS_DIR}/lib/pkgconfig/fontconfig.pc" ]; then
    echo "fontconfigの静的ビルドを行います。"
    fetch_caption_source fontconfig-2.15.0 fontconfig-2.15.0.tar.xz \
        https://www.freedesktop.org/software/fontconfig/release/fontconfig-2.15.0.tar.xz \
        63a0658d0e06e0fa886106452b58ef04f21f58202ea02a94c39de0d3335d7c0e
    (
        cd "${CAPTION_SOURCE_DIR}/fontconfig-2.15.0"
        CFLAGS="-O3 -fPIC" ./configure --prefix="${BASELIBS_DIR}" --libdir="${BASELIBS_DIR}/lib" \
            --enable-static --disable-shared --with-pic --disable-docs --disable-cache-build \
            --sysconfdir=/etc --localstatedir=/var
        make -j"${CAPTION_JOBS}"
        # /etc/fontsと/varは実行環境の設定を使い、ビルド時には変更しない。
        make install DESTDIR="${CAPTION_SOURCE_DIR}/fontconfig-2.15.0/_install"
        cp -a "${CAPTION_SOURCE_DIR}/fontconfig-2.15.0/_install${BASELIBS_DIR}/." "${BASELIBS_DIR}/"
    )
fi

if [ ! -f "${BASELIBS_DIR}/lib/libaribcaption.a" ] || [ ! -f "${BASELIBS_DIR}/lib/pkgconfig/libaribcaption.pc" ]; then
    echo "libaribcaptionの静的ビルドを行います。"
    fetch_caption_source libaribcaption-1.1.2 libaribcaption-1.1.2.tar.gz \
        https://codeload.github.com/xqq/libaribcaption/tar.gz/refs/tags/v1.1.2 \
        649b50bde99272b97c66af2a8400163e2f84eae072d252daa26baaaf0866a1c2
    cmake -S "${CAPTION_SOURCE_DIR}/libaribcaption-1.1.2" -B "${CAPTION_SOURCE_DIR}/libaribcaption-1.1.2/_build" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_INSTALL_PREFIX="${BASELIBS_DIR}" -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_PREFIX_PATH="${BASELIBS_DIR}" -DARIBCC_SHARED_LIBRARY=OFF \
        -DARIBCC_USE_FREETYPE=ON -DARIBCC_USE_FONTCONFIG=ON -DARIBCC_USE_EMBEDDED_FREETYPE=OFF \
        -DFREETYPE_INCLUDE_DIR_freetype2="${BASELIBS_DIR}/include/freetype2" \
        -DFREETYPE_INCLUDE_DIR_ft2build="${BASELIBS_DIR}/include/freetype2" \
        -DFREETYPE_LIBRARY_RELEASE="${BASELIBS_DIR}/lib/libfreetype.a" \
        -DFontconfig_INCLUDE_DIR="${BASELIBS_DIR}/include" \
        -DFontconfig_LIBRARY="${BASELIBS_DIR}/lib/libfontconfig.a"
    cmake --build "${CAPTION_SOURCE_DIR}/libaribcaption-1.1.2/_build" --parallel "${CAPTION_JOBS}"
    cmake --install "${CAPTION_SOURCE_DIR}/libaribcaption-1.1.2/_build"
fi
