#!/bin/bash

# C#プロジェクトのビルド用スクリプト
# Usage: ./scripts/build-csharp.sh convert|build ProjectName ProjectType TargetFramework OutputFile

ACTION=$1
PROJECT_NAME=$2
PROJECT_TYPE=$3        # classlib または console または wpf
TARGET_FRAMEWORK=$4    # net6.0 または net6.0-windows など
OUTPUT_FILE=$5         # 出力ファイルのパス（ビルド時のみ使用）

PROJECT_DIR="../$PROJECT_NAME"
SDK_PROJECT_FILE="$PROJECT_NAME.csproj.sdk"
PROJ_FILE="$PROJECT_NAME.csproj"

# 共通の設定
LOG4NET_VERSION="2.0.15"
TPL_DATAFLOW_VERSION="4.5.24"
BASE_PROPS='    <TargetFramework>'"$TARGET_FRAMEWORK"'</TargetFramework>
    <ImplicitUsings>enable</ImplicitUsings>
    <Nullable>enable</Nullable>'

# プロジェクトファイルの共通部分を生成する関数
generate_base_project() {
    local output_type=$1
    local additional_props=$2
    local additional_items=$3

    echo '<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
'"$BASE_PROPS"'
    <OutputType>'"$output_type"'</OutputType>
'"$additional_props"'
  </PropertyGroup>
'"$additional_items"'
</Project>'
}

# パッケージ参照を生成する関数
generate_package_references() {
    local packages=$1
    if [ -n "$packages" ]; then
        echo '  <ItemGroup>
'"$packages"'
  </ItemGroup>'
    fi
}

# エラーチェック
if [ -z "$ACTION" ] || [ -z "$PROJECT_NAME" ] || [ -z "$PROJECT_TYPE" ] || [ -z "$TARGET_FRAMEWORK" ]; then
    echo "引数が不足しています"
    echo "Usage: ./scripts/build-csharp.sh convert|build ProjectName ProjectType TargetFramework [OutputFile]"
    exit 1
fi

if [ "$ACTION" != "convert" ] && [ "$ACTION" != "build" ]; then
    echo "ACTIONには convert または build を指定してください"
    exit 1
fi

if [ "$ACTION" = "build" ] && [ -z "$OUTPUT_FILE" ]; then
    echo "ビルドモードでは出力ファイルパスが必要です"
    exit 1
fi

# プロジェクトディレクトリに移動
cd "$PROJECT_DIR" || { echo "ディレクトリ $PROJECT_DIR が見つかりません"; exit 1; }

# プロジェクト変換処理
if [ "$ACTION" = "convert" ]; then
    if [ ! -f "$SDK_PROJECT_FILE" ]; then
        echo "SDKスタイルのプロジェクトファイルを作成します: $SDK_PROJECT_FILE"

        # プロジェクトタイプに応じたテンプレート作成
        if [ "$PROJECT_TYPE" = "classlib" ]; then
            dotnet new classlib -n "$PROJECT_NAME" -o . -f "$TARGET_FRAMEWORK"
            rm -f Class1.cs
            
            additional_props="    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>"
            
            if [ "$PROJECT_NAME" = "AmatsukazeServer" ]; then
                additional_props="$additional_props
    <RootNamespace>Amatsukaze</RootNamespace>"
                
                additional_items='  <ItemGroup>
    <None Update="Log4net.Config.xml">
      <CopyToOutputDirectory>PreserveNewest</CopyToOutputDirectory>
    </None>
  </ItemGroup>
'"$(generate_package_references '    <PackageReference Include="log4net" Version="'"$LOG4NET_VERSION"'" />
    <PackageReference Include="Microsoft.Tpl.Dataflow" Version="4.5.24" />')"
                
                generate_base_project "Library" "$additional_props" "$additional_items" > "$SDK_PROJECT_FILE"
            else
                generate_base_project "Library" "$additional_props" "" > "$SDK_PROJECT_FILE"
            fi
            
        elif [ "$PROJECT_TYPE" = "console" ]; then
            dotnet new console -n "$PROJECT_NAME" -o . -f "$TARGET_FRAMEWORK"
            rm -f Program.cs
            
            if [ "$PROJECT_NAME" = "AmatsukazeServerCLI" ] || [ "$PROJECT_NAME" = "AmatsukazeAddTask" ]; then
                additional_items='  <ItemGroup>
    <ProjectReference Include="../AmatsukazeServer/AmatsukazeServer.csproj" />
  </ItemGroup>
'"$(generate_package_references '    <PackageReference Include="log4net" Version="'"$LOG4NET_VERSION"'" />')"
                
                generate_base_project "Exe" "" "$additional_items" > "$SDK_PROJECT_FILE"
            else
                generate_base_project "Exe" "" "" > "$SDK_PROJECT_FILE"
            fi
            
        elif [ "$PROJECT_TYPE" = "wpf" ]; then
            dotnet new wpf -n "$PROJECT_NAME" -o . -f "$TARGET_FRAMEWORK"
            rm -f MainWindow.xaml MainWindow.xaml.cs App.xaml App.xaml.cs
            
            additional_props="    <UseWPF>true</UseWPF>
    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>
    <RootNamespace>Amatsukaze</RootNamespace>"
            
            additional_items='  <ItemGroup>
    <ProjectReference Include="../AmatsukazeServer/AmatsukazeServer.csproj" />
  </ItemGroup>
'"$(generate_package_references '    <PackageReference Include="log4net" Version="'"$LOG4NET_VERSION"'" />
    <PackageReference Include="Microsoft.Tpl.Dataflow" Version="'"$TPL_DATAFLOW_VERSION"'" />')"
            
            generate_base_project "WinExe" "$additional_props" "$additional_items" > "$SDK_PROJECT_FILE"
            
        else
            echo "未対応のプロジェクトタイプ: $PROJECT_TYPE"
            exit 1
        fi
    else
        echo "既にSDKプロジェクトファイルが存在します: $SDK_PROJECT_FILE"
    fi
    
    echo "変換完了"
    
    # 出力ファイルの生成（メソン用）
    touch "$OUTPUT_FILE"

# ビルド処理
elif [ "$ACTION" = "build" ]; then
    if [ ! -f "$SDK_PROJECT_FILE" ]; then
        echo "SDKプロジェクトファイルが見つかりません: $SDK_PROJECT_FILE"
        exit 1
    fi
    
    echo "ビルドを開始します: $PROJECT_NAME"
    cp "$SDK_PROJECT_FILE" "$PROJ_FILE"
    
    # ビルド実行
    dotnet build -c Release
    
    # 出力ファイルコピー
    if [ "$PROJECT_TYPE" = "classlib" ]; then
        cp "bin/Release/$TARGET_FRAMEWORK/$PROJECT_NAME.dll" "$OUTPUT_FILE"
    elif [ "$PROJECT_TYPE" = "wpf" ]; then
        # WPFアプリケーションの場合
        cp "bin/Release/$TARGET_FRAMEWORK/$PROJECT_NAME.exe" "$OUTPUT_FILE"
        cp "bin/Release/$TARGET_FRAMEWORK/$PROJECT_NAME.dll" "$(dirname "$OUTPUT_FILE")/"
        # 依存ファイルもコピー
        cp "bin/Release/$TARGET_FRAMEWORK/"*.dll "$(dirname "$OUTPUT_FILE")/"
        cp "bin/Release/$TARGET_FRAMEWORK/"*.json "$(dirname "$OUTPUT_FILE")/" 2>/dev/null || :
        cp "bin/Release/$TARGET_FRAMEWORK/"*.config "$(dirname "$OUTPUT_FILE")/" 2>/dev/null || :
    else
        # 通常のコンソールアプリケーション
        cp "bin/Release/$TARGET_FRAMEWORK/$PROJECT_NAME" "$OUTPUT_FILE"
    fi
    
    # 一時ファイル削除
    rm -f "$PROJ_FILE"
    
    echo "ビルド完了"
fi

exit 0 