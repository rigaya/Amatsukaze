# AmatsukazeAddTask meson build file
project_name = 'AmatsukazeAddTask'

# AmatsukazeServerへの依存関係を追加
# CustomTargetオブジェクトを直接参照
amatsukaze_server_target = build_target_server

if use_dotnet
  # .NET でのビルド（SDKスタイルプロジェクト使用）
  build_target = custom_target(project_name,
    output : project_name,
    command : [
      'dotnet', 'build',
      '-c', 'Release',
      meson.current_source_dir() / project_name + '.csproj',
      '-o', meson.current_build_dir(),
    ],
    depends : [amatsukaze_server_target],
    build_by_default : true
  )
else
  # Monoでのビルド
  # AmatsukazeServerの参照を修正
  amatsukaze_server_dll = meson.project_build_root() / 'AmatsukazeServer' / 'AmatsukazeServer.dll'
  
  build_target = custom_target(project_name,
    output : project_name,
    command : [
      'sh', '-c',
      '''
      cd @CURRENT_SOURCE_DIR@ && \\
      mcs -out:@OUTPUT@ -r:System.dll,System.Core.dll,@0@ *.cs Properties/*.cs && \\
      chmod +x @OUTPUT@
      '''.format(amatsukaze_server_dll)
    ],
    depends : [amatsukaze_server_target],
    build_by_default : true
  )
endif 