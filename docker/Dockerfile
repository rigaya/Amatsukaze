# 配布アーカイブを使用するLinuxコンテナ
FROM ubuntu:24.04 AS fdkaac-builder

ARG FDK_AAC_REV=d8e6b1a3aa606c450241632b64b703f21ea31ce3
ARG FDKAAC_VER=v1.0.6

# fdkaacは配布アーカイブに含めず、ソースからビルドする
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential ca-certificates git autoconf automake libtool pkg-config \
    && git clone https://github.com/mstorsjo/fdk-aac.git /tmp/fdk-aac \
    && cd /tmp/fdk-aac \
    && git checkout "$FDK_AAC_REV" \
    && ./autogen.sh \
    && ./configure --disable-shared --prefix=/tmp/fdk-aac-libs \
    && make -j"$(nproc)" \
    && make install \
    && git clone --depth=1 --branch "$FDKAAC_VER" https://github.com/nu774/fdkaac.git /tmp/fdkaac \
    && cd /tmp/fdkaac \
    && autoreconf -i \
    && PKG_CONFIG_PATH=/tmp/fdk-aac-libs/lib/pkgconfig ./configure --prefix=/tmp/fdkaac-install \
    && make -j"$(nproc)" \
    && make install \
    && mkdir -p /fdkaac-output \
    && install -D /tmp/fdkaac-install/bin/fdkaac /fdkaac-output/fdkaac \
    && rm -rf /var/lib/apt/lists/*

# Battlemage等の新しいGPUとMesaのOpenCL (rusticl) を使うため、Ubuntu 24.04をベースにする
# Amatsukaze本体のCUDAはcudartを静的リンクしており、ホストのlibcuda.so.1だけを使うため、
# nvidia/cudaのベースイメージは不要 (NVIDIA_REQUIRE_CUDAで古いドライバが弾かれるのも避けられる)
FROM ubuntu:24.04

ARG ARCH=amd64
ARG UBUNTU_NAME=noble
# VCEEncCは常にVA-API (Mesa) で導入し、AMFは任意とする
ARG ENABLE_VCEENC_AMF=0
ARG QSVENCC_VER=8.32
ARG NVENCC_VER=9.37
ARG VCEENCC_VER=9.21
# NVEncCのnvrtc用。24.04向けリポジトリに11.8は無いため、22.04向けのdebを直接導入する
# NVEncCは無印のlibnvrtc.soをdlopenするが、このdebには含まれないためリンクを追加する
ARG CUDA_NVRTC_DEB=cuda-nvrtc-11-8_11.8.89-1_amd64.deb
ARG CUDA_NVRTC_SHA256=6f083fcd63b7f8ef7478bfb7b8aba938d8ca0b5684d610df1243302b12a54f06
# Intel GPUのドライバ類はUbuntu向けのkobuk-team PPAから導入する
# Haswell以前はi965のみ対応のため、QSVEncCのVA-APIバックエンド用にi965も導入する
ARG INTEL_GRAPHICS_PPA_KEY=0C0E6AF955CE463C03FC51574D098D70AFBE5E1F
# PPAのintel-opencl-icdはGen12以降のみ対応のため、Gen11以前 (Kaby Lake等) 向けに
# Intel公式のlegacy1ランタイムを追加する (別パッケージ・別ICDのため併存できる)
ARG INTEL_IGC_LEGACY_VER=1.0.17537.24
ARG INTEL_IGC_CORE_LEGACY_SHA256=c1e1ecdfe2064c047c552651cfdcdafc504f2033afafba65654338b880048b67
ARG INTEL_IGC_OPENCL_LEGACY_SHA256=dd016400f87fa2b6a9fa9fbcca7eb4a2629174a29de679709f9bec5cede88b0e
ARG INTEL_OPENCL_LEGACY1_VER=24.35.30872.36
ARG INTEL_OPENCL_LEGACY1_SHA256=bbe71e4f414259e06a10cde72c29a2bd78d41b2bb2f6f8463b1806797fe66e85
ARG AMDGPU_INSTALL_VER=25.35.1
ARG AMDGPU_INSTALL_DEB_VER=7.2.1.70201-1

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Asia/Tokyo
ENV LANG=ja_JP.UTF-8
ENV LC_ALL=ja_JP.UTF-8
ENV LC_CTYPE=ja_JP.UTF-8
ENV PATH="/app/exe_files:$PATH"
# NVIDIA Container ToolkitにGPUとドライバ機能 (CUDA/NVENC/NVDEC) の注入を要求する
ENV NVIDIA_VISIBLE_DEVICES=all
ENV NVIDIA_DRIVER_CAPABILITIES=compute,utility,video

# 同梱されないPython、字幕用フォントとGPU用の実行時依存を導入する
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates curl wget gpg tzdata locales gosu python3 xz-utils \
        fontconfig fontconfig-config fonts-noto-cjk \
    && curl -fsSL "https://keyserver.ubuntu.com/pks/lookup?op=get&search=0x${INTEL_GRAPHICS_PPA_KEY}" \
        | gpg --yes --dearmor --output /usr/share/keyrings/intel-graphics-ppa.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/intel-graphics-ppa.gpg] https://ppa.launchpadcontent.net/kobuk-team/intel-graphics/ubuntu $UBUNTU_NAME main" \
        > /etc/apt/sources.list.d/intel-graphics-ppa.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        intel-media-va-driver-non-free intel-opencl-icd libigfxcmrt7 \
        libmfx1 libmfx-gen1.2 libva-drm2 libva-x11-2 ocl-icd-libopencl1 \
        mesa-va-drivers mesa-opencl-icd i965-va-driver-shaders \
    && wget -q "https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2204/x86_64/${CUDA_NVRTC_DEB}" -O /tmp/cuda-nvrtc.deb \
    && echo "${CUDA_NVRTC_SHA256}  /tmp/cuda-nvrtc.deb" | sha256sum -c - \
    && apt-get install -y --no-install-recommends /tmp/cuda-nvrtc.deb \
    && ln -sf libnvrtc.so.11.2 /usr/local/cuda-11.8/targets/x86_64-linux/lib/libnvrtc.so \
    && echo /usr/local/cuda-11.8/targets/x86_64-linux/lib > /etc/ld.so.conf.d/cuda-nvrtc.conf \
    && ldconfig \
    && wget -q "https://github.com/intel/intel-graphics-compiler/releases/download/igc-${INTEL_IGC_LEGACY_VER}/intel-igc-core_${INTEL_IGC_LEGACY_VER}_amd64.deb" -O /tmp/intel-igc-core.deb \
    && wget -q "https://github.com/intel/intel-graphics-compiler/releases/download/igc-${INTEL_IGC_LEGACY_VER}/intel-igc-opencl_${INTEL_IGC_LEGACY_VER}_amd64.deb" -O /tmp/intel-igc-opencl.deb \
    && wget -q "https://github.com/intel/compute-runtime/releases/download/${INTEL_OPENCL_LEGACY1_VER}/intel-opencl-icd-legacy1_${INTEL_OPENCL_LEGACY1_VER}_amd64.deb" -O /tmp/intel-opencl-icd-legacy1.deb \
    && printf '%s  %s\n' \
        "${INTEL_IGC_CORE_LEGACY_SHA256}" /tmp/intel-igc-core.deb \
        "${INTEL_IGC_OPENCL_LEGACY_SHA256}" /tmp/intel-igc-opencl.deb \
        "${INTEL_OPENCL_LEGACY1_SHA256}" /tmp/intel-opencl-icd-legacy1.deb | sha256sum -c - \
    && apt-get install -y --no-install-recommends \
        /tmp/intel-igc-core.deb /tmp/intel-igc-opencl.deb /tmp/intel-opencl-icd-legacy1.deb \
    && ldconfig \
    && wget -q "https://github.com/rigaya/QSVEnc/releases/download/${QSVENCC_VER}/qsvencc_${QSVENCC_VER}_${ARCH}.deb" -O /tmp/qsvencc.deb \
    && apt-get install -y --no-install-recommends /tmp/qsvencc.deb \
    && wget -q "https://github.com/rigaya/NVEnc/releases/download/${NVENCC_VER}/nvencc_${NVENCC_VER}_${ARCH}.deb" -O /tmp/nvencc.deb \
    && apt-get install -y --no-install-recommends /tmp/nvencc.deb \
    && wget -q "https://github.com/rigaya/VCEEnc/releases/download/${VCEENCC_VER}/vceencc_${VCEENCC_VER}_${ARCH}.deb" -O /tmp/vceencc.deb \
    && apt-get install -y --no-install-recommends /tmp/vceencc.deb \
    && if [ ! -e /lib/x86_64-linux-gnu/libOpenCL.so ] && [ -e /lib/x86_64-linux-gnu/libOpenCL.so.1 ]; then \
        ln -s /lib/x86_64-linux-gnu/libOpenCL.so.1 /lib/x86_64-linux-gnu/libOpenCL.so; \
    fi \
    && if [ "$ENABLE_VCEENC_AMF" = "1" ]; then \
        wget -q "https://repo.radeon.com/amdgpu-install/${AMDGPU_INSTALL_VER}/ubuntu/${UBUNTU_NAME}/amdgpu-install_${AMDGPU_INSTALL_DEB_VER}_all.deb" -O /tmp/amdgpu-install.deb \
        && apt-get update \
        && apt-get install -y --no-install-recommends software-properties-common /tmp/amdgpu-install.deb \
        && amdgpu-install -y --usecase=opencl --opencl=rocr --no-dkms \
        && wget -q https://repo.radeon.com/amdgpu/6.4.4/ubuntu/pool/proprietary/a/amdgpu-pro-core/amdgpu-pro-core_25.10-2203192.24.04_all.deb -O /tmp/amdgpu-pro-core.deb \
        && wget -q https://repo.radeon.com/amdgpu/6.4.4/ubuntu/pool/proprietary/liba/libamdenc-amdgpu-pro/libamdenc-amdgpu-pro_25.10-2203192.24.04_amd64.deb -O /tmp/libamdenc-amdgpu-pro.deb \
        && wget -q https://repo.radeon.com/amdgpu/6.4.4/ubuntu/pool/proprietary/a/amf-amdgpu-pro/amf-amdgpu-pro_1.4.37-2203192.24.04_amd64.deb -O /tmp/amf-amdgpu-pro.deb \
        && (apt-get remove --purge -y amf-amdgpu-pro libamdenc-amdgpu-pro || true) \
        && apt-get install -y --allow-downgrades \
            /tmp/amdgpu-pro-core.deb /tmp/libamdenc-amdgpu-pro.deb /tmp/amf-amdgpu-pro.deb \
        && apt-mark hold amdgpu-pro-core libamdenc-amdgpu-pro amf-amdgpu-pro \
        && rm -f /tmp/amdgpu-install.deb /tmp/amdgpu-pro-core.deb \
            /tmp/libamdenc-amdgpu-pro.deb /tmp/amf-amdgpu-pro.deb; \
    fi \
    && fc-cache -f \
    && locale-gen ja_JP.UTF-8 en_US.UTF-8 \
    && ln -snf /usr/share/zoneinfo/$TZ /etc/localtime \
    && echo "$TZ" > /etc/timezone \
    && rm -f /tmp/intel-igc-core.deb /tmp/intel-igc-opencl.deb /tmp/intel-opencl-icd-legacy1.deb \
    && rm -f /tmp/cuda-nvrtc.deb /tmp/qsvencc.deb /tmp/nvencc.deb /tmp/vceencc.deb \
    && rm -rf /var/lib/apt/lists/*

# Amatsukaze本体を展開する
ARG AMATSUKAZE_CACHE_BUST=
# ビルド段階で生成したfdkaacだけを実行イメージへ引き継ぐ
COPY --from=fdkaac-builder /fdkaac-output/ /usr/local/bin/
RUN : "$AMATSUKAZE_CACHE_BUST" \
    && mkdir -p /app \
    && ARCHIVE_URL=$(curl -fsSL https://api.github.com/repos/rigaya/Amatsukaze/releases/latest \
        | sed -n 's/.*"browser_download_url":[[:space:]]*"\([^"]*\.tar\.xz\)".*/\1/p' \
        | grep 'Amatsukaze_linux_.*_x64\.tar\.xz' | head -1) \
    && test -n "$ARCHIVE_URL" \
    && curl -fL --retry 3 "$ARCHIVE_URL" | tar -xJ -C /app \
    && grep -Fq '"complete":true' /app/basepkg-manifest.json \
    && mkdir -p /app/bat /app/config /app/recorded /app/logo /app/output /app/profile /app/temp

# ローカルアーカイブでデバッグするときだけ、ファイル名を合わせて次の2命令を有効にする
#COPY Amatsukaze_linux_trial_x64.tar.xz /tmp/amatsukaze-debug.tar.xz
#RUN tar -xJf /tmp/amatsukaze-debug.tar.xz -C /app \
#    && grep -Fq '"complete":true' /app/basepkg-manifest.json \
#    && rm /tmp/amatsukaze-debug.tar.xz

# Faster-Whisper-XXLを展開する
ARG ENABLE_FASTER_WHISPER=0
ARG FASTER_WHISPER_VER=r245.4
RUN if [ "$ENABLE_FASTER_WHISPER" = "1" ]; then \
        wget -q "https://github.com/Purfview/whisper-standalone-win/releases/download/Faster-Whisper-XXL/Faster-Whisper-XXL_${FASTER_WHISPER_VER}_linux.7z" -O /tmp/faster-whisper.7z \
        && mkdir -p /opt \
        && /app/exe_files/7z/7zzs x /tmp/faster-whisper.7z -o/opt/ \
        && chmod 755 /opt/Faster-Whisper-XXL/faster-whisper-xxl \
        && ln -s /opt/Faster-Whisper-XXL /app/exe_files/Faster-Whisper-XXL \
        && rm -f /tmp/faster-whisper.7z; \
    fi

WORKDIR /app

# 起動時にホストのUIDとGIDへ合わせる
RUN cat <<'ENTRYPOINT_SCRIPT' > /app/entrypoint.sh && chmod +x /app/entrypoint.sh
#!/bin/bash
set -e
umask 022

DEFAULT_UID=${RUN_UID:-1000}
DEFAULT_GID=${RUN_GID:-1000}
if ! getent group "$DEFAULT_GID" >/dev/null 2>&1; then groupadd -g "$DEFAULT_GID" appgroup; fi
if ! getent passwd "$DEFAULT_UID" >/dev/null 2>&1; then useradd -u "$DEFAULT_UID" -g "$DEFAULT_GID" -m -s /bin/bash appuser; fi

USER_NAME=$(id -nu "$DEFAULT_UID")
# 複数GPUを並列で使えるよう、渡されたすべてのGPUデバイスの所有グループに参加させる
for GPU_NODE in /dev/dri/renderD* /dev/dri/card* /dev/kfd; do
    [ -c "$GPU_NODE" ] || continue
    GPU_GID=$(stat -c "%g" "$GPU_NODE")
    GPU_GROUP=$(getent group "$GPU_GID" | cut -d: -f1 || true)
    if [ -z "$GPU_GROUP" ]; then
        GPU_GROUP="gpu$GPU_GID"
        groupadd -g "$GPU_GID" "$GPU_GROUP"
    fi
    usermod -aG "$GPU_GROUP" "$USER_NAME"
done
chown -R --no-dereference "$DEFAULT_UID:$DEFAULT_GID" /app
if [ -d /opt/Faster-Whisper-XXL ]; then
    find /opt/Faster-Whisper-XXL -type d -exec chown "$DEFAULT_UID:$DEFAULT_GID" {} +
fi
if [ -x /app/exe_files/AmatsukazeDockerConfigFix ]; then
    gosu "$DEFAULT_UID" /app/exe_files/AmatsukazeDockerConfigFix \
        /app/config/AmatsukazeServer.xml /app/exe_files || \
        echo "警告: Docker用実行ファイル設定の補正に失敗しました" >&2
fi
exec gosu "$DEFAULT_UID" "$@"
ENTRYPOINT_SCRIPT

ENTRYPOINT ["./entrypoint.sh"]
CMD ["./AmatsukazeServer.sh"]
