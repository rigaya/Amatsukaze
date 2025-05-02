# AmatsukazeServer meson build file
project_name = 'AmatsukazeServer'

if use_dotnet
  # .NET でのビルド（SDKスタイルプロジェクト使用）
  build_target = custom_target(project_name,
    output : project_name + '.dll',
    command : [
      'dotnet', 'build',
      '-c', 'Release',
      meson.current_source_dir() / project_name + '.csproj',
      '-o', meson.current_build_dir(),
    ],
    build_by_default : true
  )
else
  # Monoでのビルド
  build_target = custom_target(project_name,
    output : project_name + '.dll',
    command : [
      'sh', '-c',
      '''
      cd @CURRENT_SOURCE_DIR@ && \\
      mcs -target:library -out:@OUTPUT@ -r:System.dll,System.Core.dll,System.Xml.dll,System.Net.Http.dll,System.Runtime.Serialization.dll,System.Data.dll -unsafe *.cs Lib/*.cs Server/*.cs Properties/*.cs
      '''
    ],
    build_by_default : true
  )
endif

# 依存関係を公開
amatsukaze_server_dep = declare_dependency(
  link_with : build_target,
  include_directories : include_directories('.')
)

# 他のプロジェクトが依存できるようにターゲットを公開
meson.override_dependency(project_name, amatsukaze_server_dep)

# ビルドターゲットを他のソースファイルから参照できるようにする
build_target_server = build_target

# メインのmeson.buildファイルでアクセスできるようにグローバル変数を設定
AmatsukazeServer_build_target_server = build_target 