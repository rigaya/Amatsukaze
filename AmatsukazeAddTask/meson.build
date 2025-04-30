# AmatsukazeAddTask meson build file
project_name = 'AmatsukazeAddTask'

# AmatsukazeServerの参照を修正
amatsukaze_server_target = meson.project_build_root() / 'AmatsukazeServer' / 'AmatsukazeServer.dll'

if use_dotnet
  # .NET 6/7/8でのビルド
  
  # プロジェクトをSDKスタイルに変換
  convert_target = custom_target('convert_to_sdk_' + project_name,
    output : project_name + '.converted',
    command : [
      scripts_dir / 'build-csharp.sh',
      'convert',
      project_name,
      'console',
      'net6.0'
    ],
    build_by_default : true
  )
  
  # .NETプロジェクトのビルド
  build_target = custom_target(project_name,
    input : convert_target,
    output : project_name,
    command : [
      scripts_dir / 'build-csharp.sh',
      'build',
      project_name,
      'console',
      'net6.0',
      '@OUTPUT@'
    ],
    build_by_default : true
  )
else
  # Monoでのビルド
  build_target = custom_target(project_name,
    output : project_name,
    command : [
      'sh', '-c',
      '''
      cd @CURRENT_SOURCE_DIR@ && \\
      mcs -out:@OUTPUT@ -r:System.dll,System.Core.dll,@0@ *.cs Properties/*.cs && \\
      chmod +x @OUTPUT@
      '''.format(amatsukaze_server_target)
    ],
    build_by_default : true
  )
endif 