#!/bin/bash

# Amatsukaze Docker ビルドスクリプト
# リリースモードとデバッグモードの両方に対応

set -e

# デフォルト値の設定
BUILD_MODE="release"
IMAGE_NAME="amatsukaze"
TAG="latest"
UBUNTU_VERSION="24.04"
ARCH="amd64"
NO_CACHE=false

# ヘルプメッセージ
show_help() {
    cat << EOF
Amatsukaze Docker ビルドスクリプト

使用方法:
    $0 [オプション]

オプション:
    -m, --mode MODE        ビルドモード (release|debug) [デフォルト: release]
    -n, --name NAME        イメージ名 [デフォルト: amatsukaze]
    -t, --tag TAG          タグ [デフォルト: latest]
    -u, --ubuntu VERSION   Ubuntuバージョン [デフォルト: 24.04]
    -a, --arch ARCH        アーキテクチャ [デフォルト: amd64]
    --no-cache             キャッシュを使用しない
    -h, --help             このヘルプを表示

例:
    # リリースモードでビルド
    $0

    # デバッグモードでビルド
    $0 --mode debug

    # 特定のタグでビルド
    $0 --tag v1.0.0

    # キャッシュなしでビルド
    $0 --no-cache
EOF
}

# 引数の解析
while [[ $# -gt 0 ]]; do
    case $1 in
        -m|--mode)
            BUILD_MODE="$2"
            shift 2
            ;;
        -n|--name)
            IMAGE_NAME="$2"
            shift 2
            ;;
        -t|--tag)
            TAG="$2"
            shift 2
            ;;
        -u|--ubuntu)
            UBUNTU_VERSION="$2"
            shift 2
            ;;
        -a|--arch)
            ARCH="$2"
            shift 2
            ;;
        --no-cache)
            NO_CACHE=true
            shift
            ;;
        -h|--help)
            show_help
            exit 0
            ;;
        *)
            echo "不明なオプション: $1"
            show_help
            exit 1
            ;;
    esac
done

# ビルドモードの検証
if [[ "$BUILD_MODE" != "release" && "$BUILD_MODE" != "debug" ]]; then
    echo "エラー: ビルドモードは 'release' または 'debug' である必要があります"
    exit 1
fi

# 完全なイメージ名の構築
FULL_IMAGE_NAME="${IMAGE_NAME}:${TAG}"
if [[ "$BUILD_MODE" == "debug" ]]; then
    FULL_IMAGE_NAME="${IMAGE_NAME}:${TAG}-debug"
fi

# 共通のDockerコマンドオプション
COMMON_DOCKER_OPTS="-e UID=\$(id -u) -e GID=\$(id -g) \\
  -v \`pwd\`/bat:/app/bat \\
  -v \`pwd\`/config:/app/config \\
  -v \`pwd\`/input:/app/input \\
  -v \`pwd\`/logo:/app/logo \\
  -v \`pwd\`/output:/app/output \\
  -v \`pwd\`/profile:/app/profile \\
  -v /tmp:/tmp"

echo "=== Amatsukaze Docker ビルド ==="
echo "ビルドモード: $BUILD_MODE"
echo "イメージ名: $FULL_IMAGE_NAME"
echo "Ubuntuバージョン: $UBUNTU_VERSION"
echo "アーキテクチャ: $ARCH"
echo "キャッシュなし: $NO_CACHE"
echo "================================"

# Dockerfileの存在確認
if [[ ! -f "Dockerfile" ]]; then
    echo "エラー: Dockerfileが見つかりません"
    exit 1
fi

# ビルドコマンドの構築
BUILD_CMD="docker build"
BUILD_CMD="$BUILD_CMD --build-arg BUILD_MODE=$BUILD_MODE"
BUILD_CMD="$BUILD_CMD --build-arg UBUNTU_VERSION=$UBUNTU_VERSION"
BUILD_CMD="$BUILD_CMD --build-arg ARCH=$ARCH"
BUILD_CMD="$BUILD_CMD -t $FULL_IMAGE_NAME"

if [[ "$NO_CACHE" == "true" ]]; then
    BUILD_CMD="$BUILD_CMD --no-cache"
fi

BUILD_CMD="$BUILD_CMD ."

echo "実行コマンド: $BUILD_CMD"
echo ""

# ビルドの実行
if eval $BUILD_CMD; then
    echo ""
    echo "✅ ビルドが完了しました: $FULL_IMAGE_NAME"
    
    # 使用方法の表示
    echo ""
    echo "=== 使用方法 ==="
    if [[ "$BUILD_MODE" == "debug" ]]; then
        echo "デバッグモードでコンテナを起動:"
        echo "  docker run -it --rm $COMMON_DOCKER_OPTS $FULL_IMAGE_NAME"
        echo ""
        echo "デバッグビルドスクリプトを実行:"
        echo "  docker run -it --rm $COMMON_DOCKER_OPTS $FULL_IMAGE_NAME /app/debug-build.sh"
    else
        echo "リリースモードでコンテナを起動:"
        echo "  docker run -d --name amatsukaze $COMMON_DOCKER_OPTS $FULL_IMAGE_NAME"
        echo ""
        echo "対話的にコンテナを起動:"
        echo "  docker run -it --rm $COMMON_DOCKER_OPTS $FULL_IMAGE_NAME /bin/bash"
    fi
else
    echo "❌ ビルドに失敗しました"
    exit 1
fi
