# AmatsukazeServer meson build file
project_name = 'AmatsukazeServer'

if use_dotnet
  # .NET 6/7/8でのビルド
  
  # プロジェクトをSDKスタイルに変換
  convert_target = custom_target('convert_to_sdk_' + project_name,
    output : project_name + '.converted',
    command : [
      scripts_dir / 'build-csharp.sh',
      'convert',
      project_name,
      'classlib',
      'net6.0'
    ],
    build_by_default : true
  )
  
  # .NETプロジェクトのビルド
  build_target = custom_target(project_name,
    input : convert_target,
    output : project_name + '.dll',
    command : [
      scripts_dir / 'build-csharp.sh',
      'build',
      project_name,
      'classlib',
      'net6.0',
      '@OUTPUT@'
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

# 他のプロジェクトが依存できるようにターゲットを公開
meson.override_dependency(project_name, declare_dependency(
  link_with : build_target
)) 