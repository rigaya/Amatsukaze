project('AmatsukazeCLI', ['c', 'cpp'],
  version : '1.0.0',
  default_options : ['warning_level=1', 'cpp_std=c++17']
)

# スクリプトディレクトリの設定
scripts_dir = meson.current_source_dir() / 'scripts'

# コンパイラフラグ
cpp_args = []
if meson.get_compiler('cpp').get_id() == 'gcc' or meson.get_compiler('cpp').get_id() == 'clang'
  cpp_args += ['-pthread', '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-unknown-pragmas', '-Wno-unused-variable', '-Wno-unused-but-set-variable']
endif

# Shift-JIS文字コード用のコンパイラフラグ（特定のファイルのみに使用）
cpp_args_cp932 = cpp_args + []
if meson.get_compiler('cpp').get_id() == 'gcc' or meson.get_compiler('cpp').get_id() == 'clang'
  cpp_args_cp932 += ['-fexec-charset=cp932']
endif

# SIMD 拡張命令セット用のコンパイルフラグ
sse2_args = cpp_args + ['-msse2']
ssse3_args = cpp_args + ['-mssse3']
sse41_args = cpp_args + ['-msse4.1']
avx_args = cpp_args + ['-mavx', '-mpopcnt']
avx2_args = cpp_args + ['-mavx2', '-mfma', '-mpopcnt', '-mbmi', '-mbmi2']
avx512_args = cpp_args + ['-mavx512f', '-mavx512bw', '-mpopcnt', '-mbmi', '-mbmi2']

# 依存ライブラリ
ffmpeg_dep = [
  dependency('libavcodec', required : true),
  dependency('libavformat', required : true),
  dependency('libavutil', required : true),
  dependency('libswscale', required : true),
  dependency('libswresample', required : true),
]

# プラットフォーム依存ライブラリ
openssl_dep = dependency('', required : false)
zlib_dep = dependency('', required : false)
if host_machine.system() != 'windows'
  openssl_dep = dependency('openssl', required : true)
  zlib_dep = dependency('zlib', required : true)
endif

if host_machine.system() == 'windows'
else
  # AviSynth依存関係を追加
  avisynth_dep = dependency('avisynth', required : false)
  if not avisynth_dep.found()
    # pkg-configで見つからない場合、手動で検索を試みる
    avisynth_cpp = meson.get_compiler('cpp')
    avisynth_dep = avisynth_cpp.find_library('avisynth', required : false)
    
    if not avisynth_dep.found()
      # それでも見つからない場合、ヘッダーのみの依存関係を作成
      avisynth_inc = include_directories('/usr/include/avisynth')
      avisynth_dep = declare_dependency(include_directories : avisynth_inc)
      message('AviSynth library not found, using header-only dependency')
    else
      message('AviSynth library found via manual search')
    endif
  else
    message('AviSynth library found via pkg-config')
  endif
endif

# Linuxではシステムのlibfaadを使用する代わりに、常に内部のlibfaadを使用
subdir('libfaad')
faad_dep = libfaad_dep

# バージョン情報の取得とVersion.hの生成
git = find_program('git')

version_h = custom_target('version_h',
  output : 'Version.h',
  command : [
    'sh', '-c',
    '''
    version_full=$(git describe --tags) && \
    version_short=$(git describe --abbrev=0 --tags) && \
    version_product=$(echo $version_short | tr '.' ',') && \
    printf '#define AMATSUKAZE_VERSION "%s"' "$version_full" > @OUTPUT@ && \
    echo "" >> @OUTPUT@ && \
    echo "#define AMATSUKAZE_PRODUCTVERSION $version_product" >> @OUTPUT@
    '''
  ],
  build_by_default : true
)

# Version.hの依存関係を追加
version_dep = declare_dependency(sources: version_h)

# サブディレクトリ
subdir('common')
subdir('TVCaptionMod2/Caption_src')
subdir('Amatsukaze')
subdir('AmatsukazeCLI')

# C#プロジェクトのビルドを追加（Linux対応）
if get_option('build_csharp_projects')
  cs_compiler = find_program('csc', 'dotnet', 'mcs', required : false)
  
  if cs_compiler.found()
    message('C# compiler found, will build C# projects')
    
    # dotnetコマンドが見つかったかを確認する別の方法
    use_dotnet = false
    # 古いバージョンのmesonでも動作するようにする
    dotnet_check = run_command('sh', '-c', 'which dotnet >/dev/null 2>&1 && echo true || echo false', check : false)
    if dotnet_check.returncode() == 0 and dotnet_check.stdout().strip() == 'true'
      use_dotnet = true
      message('Using .NET Core/.NET 5+ for building C# projects')
    else
      message('Using Mono for building C# projects')
    endif
    
    if use_dotnet
      # .NET Core/.NET 5+ でのビルド
      subdir('AmatsukazeServer')
      subdir('AmatsukazeServerCLI')
      subdir('AmatsukazeAddTask')
    else
      # Monoでのビルド
      subdir('AmatsukazeServer')
      subdir('AmatsukazeServerCLI')
      subdir('AmatsukazeAddTask')
    endif
  else
    message('C# compiler not found, skipping C# projects')
  endif
endif

