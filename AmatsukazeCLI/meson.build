amatsukaze_cli_sources = [
  'AmatsukazeCLI.cpp',
]

amatsukaze_inc = include_directories('.')
amatsukaze_include_dirs = [
  amatsukaze_inc,
  include_directories('../include'),
  include_directories('../include_gpl'),
  include_directories('../common'),
]

amatsukaze_cli = executable('AmatsukazeCLI', 
  amatsukaze_cli_sources,
  include_directories : amatsukaze_include_dirs,
  cpp_args : cpp_args,
  install : true,
  link_args : [],
)