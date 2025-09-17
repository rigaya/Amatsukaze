#!/bin/bash

# Amatsukaze Docker Compose ビルド・起動スクリプト
# リリースモードとデバッグモードの両方に対応

set -e

# デフォルト値の設定
BUILD_MODE="release"
IMAGE_NAME="amatsukaze"
TAG="latest"
NO_CACHE=false

# ヘルプメッセージ
show_help() {
    cat << EOF
Amatsukaze Docker Compose ビルド・起動スクリプト

使用方法:
    $0 [オプション]

オプション:
    -m, --mode MODE        ビルドモード (release|debug) [デフォルト: release]
    -n, --name NAME        イメージ名 [デフォルト: amatsukaze]
    -t, --tag TAG          タグ [デフォルト: latest]
    --no-cache             キャッシュを使用しない
    -h, --help             このヘルプを表示

例:
    # リリースモードでビルド・起動
    $0

    # デバッグモードでビルド・起動
    $0 --mode debug

    # キャッシュなしでビルド・起動
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

# ディレクトリセットアップ関数
setup_directories() {
    echo "=== ディレクトリセットアップ ==="
    
    # build.shのディレクトリを取得
    DOCKERFILE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    DEFAULTS_DIR="$DOCKERFILE_DIR/../defaults"
    CURRENT_DIR="$(pwd)"
    
    # ディレクトリのコピー
    echo ""
    echo "--- ディレクトリのコピー ---"
    
    # avsディレクトリのコピー
    if [[ ! -d "$CURRENT_DIR/avs" ]]; then
        if [[ -d "$DEFAULTS_DIR/avs" ]]; then
            cp -r "$DEFAULTS_DIR/avs" "$CURRENT_DIR/"
            echo "✅ avsディレクトリをコピーしました"
        else
            echo "⚠️  $DEFAULTS_DIR/avs が見つかりません"
        fi
    else
        echo "ℹ️  avsディレクトリは既に存在します"
    fi
    
    # batディレクトリのコピー（bat_linuxから）
    if [[ ! -d "$CURRENT_DIR/bat" ]]; then
        if [[ -d "$DEFAULTS_DIR/bat_linux" ]]; then
            cp -r "$DEFAULTS_DIR/bat_linux" "$CURRENT_DIR/bat"
            echo "✅ batディレクトリをコピーしました"
        else
            echo "⚠️  $DEFAULTS_DIR/bat_linux が見つかりません"
        fi
    else
        echo "ℹ️  batディレクトリは既に存在します"
    fi
    
    # profileディレクトリのコピー
    if [[ ! -d "$CURRENT_DIR/profile" ]]; then
        if [[ -d "$DEFAULTS_DIR/profile" ]]; then
            cp -r "$DEFAULTS_DIR/profile" "$CURRENT_DIR/"
            echo "✅ profileディレクトリをコピーしました"
        else
            echo "⚠️  $DEFAULTS_DIR/profile が見つかりません"
        fi
    else
        echo "ℹ️  profileディレクトリは既に存在します"
    fi

    # drcsディレクトリのコピー
    if [[ ! -d "$CURRENT_DIR/drcs" ]]; then
        if [[ -d "$DEFAULTS_DIR/drcs" ]]; then
            cp -r "$DEFAULTS_DIR/drcs" "$CURRENT_DIR/"
            echo "✅ drcsディレクトリをコピーしました"
        else
            echo "⚠️  $DEFAULTS_DIR/drcs が見つかりません"
        fi
    else
        if [[ ! -f "$CURRENT_DIR/drcs/drcs_map.txt" ]]; then
            cp "$DEFAULTS_DIR/drcs/drcs_map.txt" "$CURRENT_DIR/drcs/drcs_map.txt"
            echo "✅ drcs_map.txtをコピーしました"
        else
            echo "ℹ️  drcsディレクトリは既に存在します"
        fi
    fi

    # JLディレクトリのコピー
    if [[ ! -d "$CURRENT_DIR/JL" ]]; then
        (wget -q https://github.com/tobitti0/join_logo_scp/archive/refs/tags/Ver4.1.0_Linux.tar.gz -O JL.tar.gz \
            && tar -xf JL.tar.gz \
            && mv join_logo_scp-Ver4.1.0_Linux/JL "$CURRENT_DIR/" \
            && rm -rf join_logo_scp-Ver4.1.0_Linux/ join_logo_scp-Ver4.1.0_Linux JL.tar.gz \
            && echo "✅ JLディレクトリを作成しました" \
        )
    else
        echo "ℹ️  JLディレクトリは既に存在します"
    fi
    
    # 必要なディレクトリの作成
    echo ""
    echo "--- ディレクトリの作成 ---"
    
    local dirs=("config" "input" "logo" "output")
    for dir in "${dirs[@]}"; do
        if [[ ! -d "$CURRENT_DIR/$dir" ]]; then
            mkdir -p "$CURRENT_DIR/$dir"
            echo "✅ $dirディレクトリを作成しました"
        else
            echo "ℹ️  $dirディレクトリは既に存在します"
        fi
    done
    
    echo "================================"
    echo ""
}

# 完全なイメージ名の構築
FULL_IMAGE_NAME="${IMAGE_NAME}:${TAG}"
if [[ "$BUILD_MODE" == "debug" ]]; then
    FULL_IMAGE_NAME="${IMAGE_NAME}:${TAG}-debug"
fi

# compose.ymlファイルの存在確認
if [[ ! -f "compose.yml" && ! -f "docker-compose.yml" ]]; then
    if [[ -f "compose.sample.yml" ]]; then
        echo "⚠️  compose.ymlファイルが見つかりません。compose.sample.ymlをコピーします..."
        cp compose.sample.yml compose.yml
        echo "✅ compose.ymlファイルを作成しました"
    else
        echo "❌ compose.ymlファイルまたはcompose.sample.ymlが見つかりません"
        exit 1
    fi
fi

echo "=== Amatsukaze Docker Compose ビルド・起動 ==="
echo "ビルドモード: $BUILD_MODE"
echo "イメージ名: $FULL_IMAGE_NAME"
echo "キャッシュなし: $NO_CACHE"
echo "================================"

# Dockerfileの存在確認
if [[ ! -f "Dockerfile" ]]; then
    echo "エラー: Dockerfileが見つかりません"
    exit 1
fi

# ビルドコマンドの構築
BUILD_CMD="docker compose build"
BUILD_CMD="$BUILD_CMD --build-arg BUILD_MODE=$BUILD_MODE"

if [[ "$NO_CACHE" == "true" ]]; then
    BUILD_CMD="$BUILD_CMD --no-cache"
fi

echo "実行コマンド: $BUILD_CMD"
echo ""

# ビルドの実行
if eval $BUILD_CMD; then
    echo ""
    echo "✅ ビルドが完了しました: $FULL_IMAGE_NAME"

    # ディレクトリセットアップの実行
    setup_directories
    
    echo ""
    echo "=== コンテナを起動 ==="
    if docker compose up -d; then
        echo "✅ コンテナが起動しました"
        echo ""
        echo "=== アクセス情報 ==="
        echo "Amatsukaze Server: http://localhost:32768"
        echo ""
        echo "=== その他のコマンド例 ==="
        echo "  ログ表示: docker compose logs -f"
        echo "  停止: docker compose down"
        echo "  再起動: docker compose restart"
        if [[ "$BUILD_MODE" == "debug" ]]; then
            echo "  シェル実行: docker compose exec amatsukaze /bin/bash"
        fi
    else
        echo "❌ コンテナの起動に失敗しました"
        exit 1
    fi
else
    echo "❌ ビルドに失敗しました"
    exit 1
fi
