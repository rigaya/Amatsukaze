amatsukaze_sources = [
  'AMTLogo.cpp',
  'AMTSource.cpp',
  'AdtsParser.cpp',
  'Amatsukaze.cpp',
  'AmatsukazeTestImpl.cpp',
  'AudioEncoder.cpp',
  'CMAnalyze.cpp',
  'CaptionFormatter.cpp',
  'ComputeKernel.cpp',
  'ConvertPix.cpp',
  'Encoder.cpp',
  'EncoderOptionParser.cpp',
  'FileUtils.cpp',
  'FilteredSource.cpp',
  'H264VideoParser.cpp',
  'HEVCVideoParser.cpp',
  'InterProcessComm.cpp',
  'LogoScan.cpp',
  'Mpeg2TsParser.cpp',
  'Mpeg2VideoParser.cpp',
  'Muxer.cpp',
  'NicoJK.cpp',
  'OSUtil.cpp',
  'PacketCache.cpp',
  'PerformanceUtil.cpp',
  'ProcessThread.cpp',
  'ReaderWriterFFmpeg.cpp',
  'StreamReform.cpp',
  'StreamUtils.cpp',
  'StringUtils.cpp',
  'TranscodeManager.cpp',
  'TranscodeSetting.cpp',
  'TsInfo.cpp',
  'TsSplitter.cpp',
  'WaveWriter.cpp',
]

# cp932文字コードが必要なファイル
cp932_sources = [
  'CaptionData.cpp',
]

# AVX/AVX2用ファイル
avx_sources = ['ComputeKernelAVX.cpp']
avx2_sources = ['ComputeKernelAVX2.cpp', 'ConvertPixAVX2.cpp']

amatsukaze_inc = include_directories('.')
amatsukaze_include_dirs = [
  amatsukaze_inc,
  include_directories('../include'),
  include_directories('../include_gpl'),
  include_directories('../common'),
]

amatsukaze_deps = [
  common_dep,
  caption_dep,
  faad_dep,
  ffmpeg_dep,
  avisynth_dep,
  openssl_dep,
  zlib_dep,
  version_dep,
]

# プラットフォーム依存コード
os_specific_libs = []

if host_machine.system() == 'windows'
  os_specific_libs += ['winmm', 'shlwapi', 'ws2_32']
  amatsukaze_cpp_args = cpp_args + ['-DAMTSUKAZE_API=__declspec(dllexport)']
  amatsukaze_link_args = []
else # Linux
  os_specific_libs += ['dl', 'pthread']
  amatsukaze_cpp_args = cpp_args + ['-DAMTSUKAZE_API=__attribute__((visibility("default")))', '-DWINAPI=', '-DAMATSUKAZE2DLL', '-fvisibility=hidden']
  
  # シンボルバージョニングファイルの作成
  version_script_path = meson.current_build_dir() / 'Amatsukaze.version'
  # すでに作成したファイルを使用
  if run_command('test', '-f', version_script_path, check: false).returncode() != 0
    # ファイルが存在しない場合は作成
    version_script_content = '''AMATSUKAZE_1.0 {
  global:
    InitAmatsukaze;
    GetFilterSourceInfo;
    GetTSPacket;
    GetVideoFrame;
    GetAudioFrame;
    GetCaptionFrame;
    GetStreamsInfo;
    GetVideoInfo;
    TermAmatsukaze;
    AvisynthPluginInit3;
    AmatsukazeCLI;
    InitAmatsukazeDLL;
  local:
    *;
};
'''
    configure_file(
      output: 'Amatsukaze.version',
      command: ['sh', '-c', 'echo "' + version_script_content + '" > @OUTPUT@'],
      capture: false
    )
  endif
  
  amatsukaze_link_args = ['-Wl,--version-script,' + version_script_path]
endif

# AVXとAVX2のためのカスタムターゲット
if host_machine.system() == 'windows'
  # Windows用のコンパイラフラグ
  avx_cpp_args = amatsukaze_cpp_args + ['/arch:AVX']
  avx2_cpp_args = amatsukaze_cpp_args + ['/arch:AVX2']
else
  # Linux用のコンパイラフラグ
  avx_cpp_args = amatsukaze_cpp_args + avx_args
  avx2_cpp_args = amatsukaze_cpp_args + avx2_args
endif

# AVXファイルのターゲット
avx_lib = static_library('avx_lib',
  avx_sources,
  include_directories : amatsukaze_include_dirs,
  dependencies : amatsukaze_deps,
  cpp_args : avx_cpp_args
)

# AVX2ファイルのターゲット
avx2_lib = static_library('avx2_lib',
  avx2_sources,
  include_directories : amatsukaze_include_dirs,
  dependencies : amatsukaze_deps,
  cpp_args : avx2_cpp_args
)

# AribString.cppは_UNICODEマクロ付きで別途ビルド
aribstring_lib = static_library('aribstring',
  'AribString.cpp',
  include_directories : amatsukaze_include_dirs,
  dependencies : amatsukaze_deps,
  cpp_args : amatsukaze_cpp_args + ['-D_UNICODE'] + (meson.get_compiler('cpp').get_id() == 'gcc' or meson.get_compiler('cpp').get_id() == 'clang' ? ['-fexec-charset=cp932'] : []),
  install : false,
)

# cp932文字コードが必要なファイル用のライブラリ
cp932_lib = static_library('cp932_files',
  cp932_sources,
  include_directories : amatsukaze_include_dirs,
  dependencies : amatsukaze_deps,
  cpp_args : amatsukaze_cpp_args + (meson.get_compiler('cpp').get_id() == 'gcc' or meson.get_compiler('cpp').get_id() == 'clang' ? ['-fexec-charset=cp932'] : []),
  install : false,
)

amatsukaze_lib = shared_library('Amatsukaze',
  amatsukaze_sources,
  include_directories : amatsukaze_include_dirs,
  dependencies : amatsukaze_deps,
  cpp_args : amatsukaze_cpp_args,
  link_args : amatsukaze_link_args,
  link_with : [avx_lib, avx2_lib, aribstring_lib, cp932_lib],
  install : true,
)

amatsukaze_dep = declare_dependency(
  link_with : amatsukaze_lib,
  include_directories : amatsukaze_include_dirs,
) 