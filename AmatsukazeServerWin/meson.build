# AmatsukazeServerWin meson build file
project_name = 'AmatsukazeServerWin'

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
  # Monoでのビルド - Windows専用なのでスキップ
  build_target = custom_target(project_name,
    output : project_name + '.dll',
    command : [
      'echo', 'Skipping Windows-specific build on non-Windows platform'
    ],
    build_by_default : false
  )
endif

# 他のプロジェクトが依存できるようにターゲットを公開
meson.override_dependency(project_name, declare_dependency(
  link_with : build_target
)) 