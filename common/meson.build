common_sources = [
  'cpu_info.cpp',
  'rgy_bitstream.cpp',
  'rgy_event.cpp',
  'rgy_env.cpp',
  'rgy_filesystem.cpp',
  'rgy_codepage.cpp',
  'rgy_language.cpp',
  'rgy_memmem.cpp',
  'rgy_pipe.cpp',
  'rgy_pipe_linux.cpp',
  'rgy_simd.cpp',
  'rgy_thread_affinity.cpp',
  'rgy_util.cpp',
]

# SIMD拡張命令セットを使用するソースファイル
common_avx2_sources = [
  'rgy_bitstream_avx2.cpp',
  'rgy_memmem_avx2.cpp',
]

common_avx512_sources = [
  'rgy_bitstream_avx512bw.cpp',
  'rgy_memmem_avx512bw.cpp',
]

common_inc = include_directories('.')

# 基本ソースのコンパイル
common_lib = static_library('common', 
  common_sources,
  include_directories : common_inc,
  cpp_args : cpp_args,
  pic : true,
  install : false,
)

# AVX2ソースのコンパイル
common_avx2_lib = static_library('common_avx2',
  common_avx2_sources,
  include_directories : common_inc,
  cpp_args : avx2_args,
  pic : true,
  install : false,
)

# AVX512ソースのコンパイル
common_avx512_lib = static_library('common_avx512',
  common_avx512_sources,
  include_directories : common_inc,
  cpp_args : avx512_args,
  pic : true,
  install : false,
)

common_dep = declare_dependency(
  link_with : [common_lib, common_avx2_lib, common_avx512_lib],
  include_directories : common_inc,
)