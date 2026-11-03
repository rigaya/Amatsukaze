#!/bin/bash

# Amatsukaze Docker Compose セットアップスクリプト
# 必要なディレクトリの構成と設定ファイルの準備を行います

set -e

# ヘルプメッセージ
show_help() {
    cat << EOF
Amatsukaze Docker Compose セットアップスクリプト

使用方法:
    $0 [オプション]

オプション:
    -h, --help             このヘルプを表示
    --edcb-dir <パス>       EDCB-Linuxの設定ディレクトリ(既定: /var/local/edcb)
    --uid <UID>            実行ユーザーID
    --gid <GID>            実行グループID
    --groups <GID,...>     補助グループID

このスクリプトは以下の処理を行います:
    1. 必要なディレクトリの作成
    2. 設定ファイルのコピー
    3. compose.ymlファイルの準備

セットアップ完了後は以下のコマンドを実行してください:
    docker compose up -d

例:
    # セットアップの実行
    $0

    # セットアップ完了後の起動
    docker compose up -d
EOF
}

EDCB_DIR="/var/local/edcb"
uid_override=false
groups_override=false
run_uid="${SUDO_UID:-$(${AMT_SETUP_TEST_ID_CMD:-id} -u)}"
run_gid="${SUDO_GID:-$(${AMT_SETUP_TEST_ID_CMD:-id} -g)}"
run_groups=""

# 引数の解析
while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            show_help
            exit 0
            ;;
        --edcb-dir)
            if [[ $# -lt 2 || -z "$2" ]]; then
                echo "--edcb-dirにはパスが必要です" >&2
                exit 1
            fi
            EDCB_DIR="$2"
            shift 2
            ;;
        --uid|--gid|--groups)
            option="$1"
            if [[ $# -lt 2 || -z "$2" ]]; then echo "$optionには値が必要です" >&2; exit 1; fi
            if [[ "$option" == --groups ]]; then
                if [[ ! "$2" =~ ^[0-9]+(,[0-9]+)*$ ]] || [[ ",$2," =~ ,0+, ]]; then echo "無効な補助グループID: $2" >&2; exit 1; fi
                run_groups="$2"; groups_override=true
            else
                if [[ ! "$2" =~ ^[0-9]+$ || "$2" =~ ^0+$ ]]; then echo "無効なID: $2" >&2; exit 1; fi
                if [[ "$option" == --uid ]]; then run_uid="$2"; uid_override=true; else run_gid="$2"; fi
            fi
            shift 2
            ;;
        *)
            echo "不明なオプション: $1"
            show_help
            exit 1
            ;;
    esac
done

# EDCBの設定を読み取り、同一パスでマウントする候補を収集する
recorded_paths=()
read_edcb_settings() {
    local ini="$EDCB_DIR/Common.ini" path existing duplicate num
    local -a entries=()
    if [[ ! -f "$ini" || ! -r "$ini" ]]; then
        if [[ -e "$ini" ]]; then
            echo "警告: $iniを読み取れません"
        fi
        echo "EDCB-Linuxが見つからないため、volumesの/mnt/recorded、/mnt/encodedを実際のフォルダに合わせて編集してください"
        return
    fi
    # 読み取り失敗を検出してから解析する
    if ! cat "$ini" >/dev/null; then
        echo "警告: $iniを読み取れません"
        echo "EDCB-Linuxが見つからないため、volumesの/mnt/recorded、/mnt/encodedを実際のフォルダに合わせて編集してください"
        return
    fi
    mapfile -t entries < <(awk '
        BEGIN { section = 0; num = "" }
        NR == 1 { sub(/^\xef\xbb\xbf/, "") }
        { gsub(/\r/, ""); line = $0; sub(/^[[:space:]]+/, "", line); sub(/[[:space:]]+$/, "", line) }
        line ~ /^[;#]/ { next }
        line ~ /^\[/ { section = (tolower(line) == "[set]"); next }
        section && index(line, "=") {
            key = tolower(substr(line, 1, index(line, "=") - 1))
            value = substr(line, index(line, "=") + 1)
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", key)
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", value)
            if (key == "recfoldernum") num = value
            else if (key == "recinfofolder") info = value
            else if (key ~ /^recfolderpath[0-9]+$/) paths[substr(key, 14) + 0] = value
        }
        END {
            if (num == "") num = "0"
            print num
            if (num ~ /^[0-9]+$/ && num + 0 > 0) {
                for (i = 0; i < num + 0; i++) if (i in paths) print paths[i]
                if (info != "") print info
            }
        }
    ' "$ini")
    num="${entries[0]}"
    if [[ ! "$num" =~ ^[0-9]+$ ]]; then
        echo "警告: RecFolderNumが整数ではありません: $num"
        num=0
    fi
    for path in "${entries[@]:1}"; do
        while [[ "$path" == */ && "$path" != / ]]; do path="${path%/}"; done
        if [[ -z "$path" || "$path" != /* || "$path" == / || "$path" == *:* || "$path" == *'"'* || "$path" == *\\* || "$path" == *$'\n'* ]]; then
            echo "警告: 無効な録画フォルダをスキップします: $path"
            continue
        fi
        duplicate=false
        for existing in "${recorded_paths[@]}"; do
            [[ "$existing" == "$path" ]] && duplicate=true
        done
        if [[ "$duplicate" == false ]]; then recorded_paths+=("$path"); fi
    done
    if [[ "$num" =~ ^0+$ || ${#recorded_paths[@]} -eq 0 ]]; then
        echo "警告: EDCBの録画フォルダが未設定です(設定フォルダに録画されます)。EpgTimerSrvで録画フォルダを設定してください"
    fi
}

# Composeの変数展開とYAMLの引用を考慮してマウント行を作る
mount_line() {
    local path="$1" indent="$2"
    path="${path//\$/\$\$}"
    path="${path//\'/\'\'}"
    printf "%s- '%s:%s'\n" "$indent" "$path" "$path"
}

# テスト用のコマンド差し替えは通常利用では指定しない
setup_stat() { "${AMT_SETUP_TEST_STAT_CMD:-stat}" "$@"; }
setup_id() { "${AMT_SETUP_TEST_ID_CMD:-id}" "$@"; }
determine_permissions() {
    local path owner group mode bits member_groups="" gid
    local -a paths=("${recorded_paths[@]}" /mnt/encoded)
    if [[ ${#recorded_paths[@]} -eq 0 ]]; then paths+=(/mnt/recorded); fi
    if [[ ! "$run_uid" =~ ^[0-9]+$ || "$run_uid" =~ ^0+$ ]]; then
        echo "警告: 実行ユーザーIDが0または不正のためサンプル値を使用します"
        run_uid=""
    fi
    if [[ ! "$run_gid" =~ ^[0-9]+$ || "$run_gid" =~ ^0+$ ]]; then
        echo "警告: 実行グループIDが0または不正のためサンプル値を使用します"
        run_gid=""
    fi
    if [[ "$groups_override" == false && "$uid_override" == false && -n "$run_uid" ]]; then
        member_groups="$(setup_id -G "${SUDO_USER:-$(setup_id -un)}" 2>/dev/null || true)"
        for path in "${paths[@]}"; do
            [[ -d "$path" ]] || continue
            group="$(setup_stat -c '%g' "$path")"
            if [[ "$group" != "$run_gid" && "$group" != 0 && " $member_groups " == *" $group "* && ",$run_groups," != *",$group,"* ]]; then
                run_groups="${run_groups:+$run_groups,}$group"
            fi
        done
    fi
    # サンプル値を残す場合も実際に書き込まれる値で判定する
    for path in "${paths[@]}"; do
        [[ -d "$path" ]] || continue
        if ! read -r owner group mode < <(setup_stat -c '%u %g %a' "$path"); then continue; fi
        bits=$((8#$mode))
        if [[ "$owner" == "${run_uid:-1000}" ]]; then bits=$(( (bits >> 6) & 7 ))
        elif [[ "$group" == "${run_gid:-1000}" || ",$run_groups," == *",$group,"* ]]; then bits=$(( (bits >> 3) & 7 ))
        else bits=$((bits & 7)); fi
        if (( (bits & 5) != 5 )); then echo "警告: $path(所有者$owner:$group、モード$mode)を読み取れません"; fi
        if (( (bits & 2) == 0 )); then
            if [[ "$path" == /mnt/encoded ]]; then echo "警告: $pathに出力できません"
            else echo "警告: $pathに書き込みできないため、succeeded/failedへの入力ファイル移動は行われません。移動する場合はsudo chmod 2770 $(printf '%q' "$path")等でグループに書き込み権限を付けてください(EDCBがサブフォルダを作る場合はedcb.serviceにUMask=0002も必要)"; fi
        fi
    done
}

compose_created=false
prepare_compose() {
    local path line indent created=false
    read_edcb_settings
    determine_permissions
    if [[ ! -f compose.yml && ! -f docker-compose.yml ]]; then
        if [[ ! -f compose.sample.yml ]]; then
            echo "❌ compose.ymlファイルまたはcompose.sample.ymlが見つかりません"
            exit 1
        fi
        created=true
        # サンプルを行単位で処理し、パスをsedの置換式に渡さない
        while IFS= read -r line || [[ -n "$line" ]]; do
            if [[ "$line" == *'# amatsukaze-setup:recorded'* && ${#recorded_paths[@]} -gt 0 ]]; then
                indent="${line%%[![:space:]]*}"
                for path in "${recorded_paths[@]}"; do mount_line "$path" "$indent"; done
            elif [[ -n "$run_uid" && "$line" =~ ^([[:space:]]*-[[:space:]]*)RUN_UID= ]]; then
                printf '%sRUN_UID=%s\n' "${BASH_REMATCH[1]}" "$run_uid"
            elif [[ -n "$run_gid" && "$line" =~ ^([[:space:]]*-[[:space:]]*)RUN_GID= ]]; then
                printf '%sRUN_GID=%s\n' "${BASH_REMATCH[1]}" "$run_gid"
            elif [[ "$line" =~ ^([[:space:]]*-[[:space:]]*)RUN_GROUPS= ]]; then
                printf '%sRUN_GROUPS=%s\n' "${BASH_REMATCH[1]}" "$run_groups"
            else
                printf '%s\n' "$line"
            fi
        done < compose.sample.yml > compose.yml
        compose_created=true
        echo "✅ compose.ymlを作成しました"
    else
        echo "ℹ️ compose.ymlは既に存在するため変更しません"
    fi
    if [[ ${#recorded_paths[@]} -gt 0 ]]; then
        if [[ "$created" == true ]]; then echo "録画フォルダのマウントを書き込みました:"; else echo "volumesに追加する録画フォルダの候補:"; fi
        for path in "${recorded_paths[@]}"; do mount_line "$path" "  "; done
    fi
    echo "実行権限の設定候補: RUN_UID=${run_uid:-サンプル値} RUN_GID=${run_gid:-サンプル値} RUN_GROUPS=$run_groups"
}

# 対象が無い場合もディレクトリは自動作成しない
# 既存のcompose.ymlはサンプルと構成が異なりうるため、新規作成時のみ確認する
warn_missing_mounts() {
    local path
    [[ "$compose_created" == true ]] || return 0
    local -a paths=("${recorded_paths[@]}" /mnt/encoded)
    if [[ ${#recorded_paths[@]} -eq 0 ]]; then paths+=(/mnt/recorded); fi
    for path in "${paths[@]}"; do
        if [[ ! -d "$path" ]]; then
            echo "警告: $pathが存在しません。存在しないとDockerがroot所有で作成し、権限エラーの原因になります。事前に作成してください"
            printf "作成例: sudo install -d -o %q -g %q -m 2775 %q\n" "${run_uid:-1000}" "${run_gid:-1000}" "$path"
        fi
    done
}

# ディレクトリセットアップ関数
setup_directories() {
    echo "=== Amatsukaze Docker Compose セットアップ ==="
    echo ""
    
    # setup.shのディレクトリを取得
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    DEFAULTS_DIR="$SCRIPT_DIR/../defaults"
    CURRENT_DIR="$(pwd)"
    
    # ディレクトリのコピー
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
        echo "JLディレクトリをダウンロード中..."
        (wget -q https://github.com/yobibi/join_logo_scp/archive/refs/tags/v5.1.1.tar.gz -O JL.tar.gz \
            && tar -xf JL.tar.gz \
            && mv join_logo_scp-5.1.1/JL "$CURRENT_DIR/" \
            && rm -rf join_logo_scp-5.1.1 JL.tar.gz \
            && echo "✅ JLディレクトリを作成しました" \
        )
    else
        echo "ℹ️  JLディレクトリは既に存在します"
    fi
    
    # 必要なディレクトリの作成
    echo ""
    echo "--- ディレクトリの作成 ---"
    
    local dirs=("config" "data" "logo")
    for dir in "${dirs[@]}"; do
        if [[ ! -d "$CURRENT_DIR/$dir" ]]; then
            mkdir -p "$CURRENT_DIR/$dir"
            echo "✅ $dirディレクトリを作成しました"
        else
            echo "ℹ️  $dirディレクトリは既に存在します"
        fi
    done
    
    echo ""
    echo "--- compose.ymlファイルの準備 ---"
    
    prepare_compose
    warn_missing_mounts

    echo ""
    echo "================================"
    echo "✅ セットアップが完了しました！"
    echo ""
    echo "=== 次のステップ ==="
    echo "compose.ymlを編集して設定を行ってください"
    echo "- Amatsukazeを実行するユーザーIDとグループIDをRUN_UIDとRUN_GIDで指定"
    echo "- volumesのマウント対象等を調整"
    echo "- 環境に応じて devices や deploy を調整"
    echo ""
    echo "その後以下のコマンドを実行してコンテナを起動してください:"
    echo ""
    echo "  docker compose up -d"
    echo ""
    echo "=== その他のコマンド例 ==="
    echo "  ログ表示: docker compose logs -f"
    echo "  停止: docker compose down"
    echo "  再起動: docker compose restart"
    echo "  シェル実行: docker compose exec amatsukaze /bin/bash"
    echo ""
    echo "=== アクセス情報 ==="
    echo "Amatsukaze Server: http://localhost:32768"
    echo "================================"
}

# セットアップの実行
setup_directories
