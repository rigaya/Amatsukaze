amatsukaze_genlogo_sources = [
  'AmatsukazeGenLogo.cpp',
]

amatsukaze_genlogo_inc = include_directories('.')
amatsukaze_genlogo_include_dirs = [
  amatsukaze_genlogo_inc,
  include_directories('../include'),
  include_directories('../include_gpl'),
  include_directories('../common'),
]

amatsukaze_genlogo_deps = [
  common_dep,
]

amatsukaze_genlogo = executable('AmatsukazeGenLogo',
  amatsukaze_genlogo_sources,
  include_directories : amatsukaze_genlogo_include_dirs,
  dependencies : amatsukaze_genlogo_deps,
  cpp_args : cpp_args,
  install : true,
  link_args : [],
)
