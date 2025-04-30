# libfaad静的ライブラリのmeson.buildファイル

# ソースファイルのリスト
libfaad_sources = [
  'bits.c',
  'cfft.c',
  'common.c',
  'decoder.c',
  'drc.c',
  'drm_dec.c',
  'error.c',
  'filtbank.c',
  'hcr.c',
  'huffman.c',
  'ic_predict.c',
  'is.c',
  'lt_predict.c',
  'mdct.c',
  'mp4.c',
  'ms.c',
  'output.c',
  'pns.c',
  'ps_dec.c',
  'ps_syntax.c',
  'pulse.c',
  'rvlc.c',
  'sbr_dct.c',
  'sbr_dec.c',
  'sbr_e_nf.c',
  'sbr_fbt.c',
  'sbr_hfadj.c',
  'sbr_hfgen.c',
  'sbr_huff.c',
  'sbr_qmf.c',
  'sbr_syntax.c',
  'sbr_tf_grid.c',
  'specrec.c',
  'ssr.c',
  'ssr_fb.c',
  'ssr_ipqf.c',
  'syntax.c',
  'tns.c'
]

# ヘッダファイルのパス
libfaad_inc = include_directories('.')
libfaad_include_dirs = [
  libfaad_inc,
  include_directories('./codebook'),
  include_directories('../include_gpl'),
]

# コンパイラフラグ
libfaad_args = []
if host_machine.system() == 'windows'
  libfaad_args += ['-DWIN32', '-D_WIN32']
else
  libfaad_args += ['-DHAVE_CONFIG_H=1']
endif

# 静的ライブラリのビルド
libfaad_static = static_library('faad',
  libfaad_sources,
  include_directories : libfaad_include_dirs,
  c_args : libfaad_args,
  build_by_default : true
)

# 依存関係の定義
libfaad_dep = declare_dependency(
  link_with : libfaad_static,
  include_directories : libfaad_include_dirs
) 