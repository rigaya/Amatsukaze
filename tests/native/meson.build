amatsukaze_native_tests = executable('AmatsukazeNativeTests',
  'AmatsukazeNativeTests.cpp',
  dependencies : [amatsukaze_dep] + amatsukaze_deps,
  cpp_args : host_machine.system() == 'windows' ? ['-DAMATSUKAZE_IMPORTS'] : [],
  install : false,
)

test('AmatsukazeNativeTests', amatsukaze_native_tests,
  suite : 'native',
  timeout : 30,
)

pgs_encoder_tests = executable('PgsEncoderTests',
  'PgsEncoderTests.cpp',
  '../../Amatsukaze/PgsEncoder.cpp',
  '../../Amatsukaze/PgsPalette.cpp',
  include_directories : include_directories('../../Amatsukaze'),
  dependencies : ffmpeg_dep,
  install : false,
)

test('PgsEncoderTests', pgs_encoder_tests,
  suite : 'native',
  timeout : 60,
)

arib_caption_smoke_tests = executable('AribCaptionSmokeTests',
  'AribCaptionSmokeTests.cpp',
  dependencies : amatsukaze_dep,
  cpp_args : host_machine.system() == 'windows' ? ['-DAMATSUKAZE_IMPORTS'] : [],
  install : false,
)

test('AribCaptionSmokeTests', arib_caption_smoke_tests,
  suite : 'native',
  timeout : 30,
)
