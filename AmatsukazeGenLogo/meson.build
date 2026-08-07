amatsukaze_genlogo_sources = [
  'AmatsukazeGenLogo.cpp',
  'Pass0Support.cpp',
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

amatsukaze_genlogo_pass0_test = executable('amatsukaze_genlogo_pass0_test',
  ['Pass0Support.cpp', 'Pass0SupportTest.cpp'],
  include_directories : amatsukaze_genlogo_include_dirs,
  dependencies : amatsukaze_genlogo_deps,
  cpp_args : cpp_args,
)

test('amatsukaze_genlogo_pass0', amatsukaze_genlogo_pass0_test)
