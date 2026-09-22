# 配布アーカイブを使用するLinuxコンテナ
FROM nvidia/cuda:11.8.0-base-ubuntu22.04

ARG ARCH=amd64
ARG UBUNTU_NAME=jammy
ARG ENABLE_VCEENCC=0
ARG QSVENCC_VER=8.30
ARG NVENCC_VER=9.35
ARG VCEENCC_VER=9.17
ARG AMDGPU_INSTALL_VER=25.35.1
ARG AMDGPU_INSTALL_DEB_VER=7.2.1.70201-1

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Asia/Tokyo
ENV LANG=ja_JP.UTF-8
ENV LC_ALL=ja_JP.UTF-8
ENV LC_CTYPE=ja_JP.UTF-8
ENV PATH="/app/exe_files:$PATH"

# 同梱されないPythonとGPU用の実行時依存を導入する
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates curl wget gpg tzdata locales gosu python3 xz-utils \
    && wget -qO - https://repositories.intel.com/gpu/intel-graphics.key \
        | gpg --yes --dearmor --output /usr/share/keyrings/intel-graphics.gpg \
    && echo "deb [arch=amd64,i386 signed-by=/usr/share/keyrings/intel-graphics.gpg] https://repositories.intel.com/gpu/ubuntu $UBUNTU_NAME unified" \
        > /etc/apt/sources.list.d/intel-gpu-$UBUNTU_NAME.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        intel-media-va-driver-non-free intel-opencl-icd libigfxcmrt7 \
        libmfx1 libmfx-gen1.2 libva-drm2 libva-x11-2 ocl-icd-libopencl1 \
        cuda-nvrtc-11-8 libnpp-11-8 \
    && wget -q "https://github.com/rigaya/QSVEnc/releases/download/${QSVENCC_VER}/qsvencc_${QSVENCC_VER}_${ARCH}.deb" -O /tmp/qsvencc.deb \
    && apt-get install -y --no-install-recommends /tmp/qsvencc.deb \
    && wget -q "https://github.com/rigaya/NVEnc/releases/download/${NVENCC_VER}/nvencc_${NVENCC_VER}_${ARCH}.deb" -O /tmp/nvencc.deb \
    && apt-get install -y --no-install-recommends /tmp/nvencc.deb \
    && if [ ! -e /lib/x86_64-linux-gnu/libOpenCL.so ] && [ -e /lib/x86_64-linux-gnu/libOpenCL.so.1 ]; then \
        ln -s /lib/x86_64-linux-gnu/libOpenCL.so.1 /lib/x86_64-linux-gnu/libOpenCL.so; \
    fi \
    && if [ "$ENABLE_VCEENCC" = "1" ]; then \
        wget -q "https://repo.radeon.com/amdgpu-install/${AMDGPU_INSTALL_VER}/ubuntu/${UBUNTU_NAME}/amdgpu-install_${AMDGPU_INSTALL_DEB_VER}_all.deb" -O /tmp/amdgpu-install.deb \
        && apt-get update \
        && apt-get install -y --no-install-recommends software-properties-common /tmp/amdgpu-install.deb \
        && amdgpu-install -y --usecase=opencl --opencl=rocr --no-dkms \
        && wget -q https://repo.radeon.com/amdgpu/6.4.4/ubuntu/pool/proprietary/a/amdgpu-pro-core/amdgpu-pro-core_25.10-2202160.22.04_all.deb -O /tmp/amdgpu-pro-core.deb \
        && wget -q https://repo.radeon.com/amdgpu/6.4.4/ubuntu/pool/proprietary/liba/libamdenc-amdgpu-pro/libamdenc-amdgpu-pro_25.10-2202160.22.04_amd64.deb -O /tmp/libamdenc-amdgpu-pro.deb \
        && wget -q https://repo.radeon.com/amdgpu/6.4.4/ubuntu/pool/proprietary/a/amf-amdgpu-pro/amf-amdgpu-pro_1.4.37-2202160.22.04_amd64.deb -O /tmp/amf-amdgpu-pro.deb \
        && (apt-get remove --purge -y amf-amdgpu-pro libamdenc-amdgpu-pro || true) \
        && apt-get install -y --allow-downgrades \
            /tmp/amdgpu-pro-core.deb /tmp/libamdenc-amdgpu-pro.deb /tmp/amf-amdgpu-pro.deb \
        && apt-mark hold amdgpu-pro-core libamdenc-amdgpu-pro amf-amdgpu-pro \
        && wget -q "https://github.com/rigaya/VCEEnc/releases/download/${VCEENCC_VER}/vceencc_${VCEENCC_VER}_${ARCH}.deb" -O /tmp/vceencc.deb \
        && apt-get install -y --no-install-recommends /tmp/vceencc.deb \
        && rm -f /tmp/amdgpu-install.deb /tmp/amdgpu-pro-core.deb \
            /tmp/libamdenc-amdgpu-pro.deb /tmp/amf-amdgpu-pro.deb /tmp/vceencc.deb; \
    fi \
    && locale-gen ja_JP.UTF-8 en_US.UTF-8 \
    && ln -snf /usr/share/zoneinfo/$TZ /etc/localtime \
    && echo "$TZ" > /etc/timezone \
    && rm -f /tmp/qsvencc.deb /tmp/nvencc.deb \
    && rm -rf /var/lib/apt/lists/*

# Amatsukaze本体を展開する
ARG AMATSUKAZE_ARCHIVE=
ARG AMATSUKAZE_CACHE_BUST=
RUN --mount=type=bind,source=.,target=/build-context,readonly \
    : "$AMATSUKAZE_CACHE_BUST" \
    && mkdir -p /app \
    && if [ -n "$AMATSUKAZE_ARCHIVE" ]; then \
        tar -xf "/build-context/$AMATSUKAZE_ARCHIVE" -C /app; \
    else \
        ARCHIVE_URL=$(curl -fsSL https://api.github.com/repos/rigaya/Amatsukaze/releases/latest \
            | sed -n 's/.*"browser_download_url":[[:space:]]*"\([^"]*\.tar\.xz\)".*/\1/p' \
            | grep 'Amatsukaze_linux_.*_x64\.tar\.xz' | head -1) \
        && test -n "$ARCHIVE_URL" \
        && curl -fL --retry 3 "$ARCHIVE_URL" | tar -xJ -C /app; \
    fi \
    && grep -Fq '"complete":true' /app/basepkg-manifest.json \
    && mkdir -p /app/bat /app/config /app/recorded /app/logo /app/output /app/profile /app/temp

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

RENDER_GROUP=""
if [ -c /dev/dri/renderD128 ]; then
    RENDER_GID=$(stat -c "%g" /dev/dri/renderD128)
    RENDER_GROUP=$(getent group "$RENDER_GID" | cut -d: -f1 || true)
    if [ -z "$RENDER_GROUP" ]; then
        if getent group render >/dev/null 2>&1; then
            groupmod -o -g "$RENDER_GID" render
        else
            groupadd -g "$RENDER_GID" render
        fi
        RENDER_GROUP=render
    fi
fi

USER_NAME=$(id -nu "$DEFAULT_UID")
if getent group video >/dev/null 2>&1; then usermod -aG video "$USER_NAME"; fi
if [ -n "$RENDER_GROUP" ]; then usermod -aG "$RENDER_GROUP" "$USER_NAME"; fi
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
