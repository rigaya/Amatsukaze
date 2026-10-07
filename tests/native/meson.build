amatsukaze_native_tests = executable('AmatsukazeNativeTests',
  'AmatsukazeNativeTests.cpp',
  # THROW/StringFormatの実装 (本体DLLでは非公開のため、テストにも組み込む)
  '../../Amatsukaze/StringUtils.cpp',
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
  dependencies : ffmpeg_dep + [dependency('threads')],
  objects : pgs_avx2_objects,
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

if host_machine.system() == 'linux'
  caption_pgs_render_tests = executable('CaptionPgsRenderTests',
    'CaptionPgsRenderTests.cpp',
    include_directories : amatsukaze_include_dirs,
    dependencies : amatsukaze_deps,
    cpp_args : ['-ffunction-sections', '-fdata-sections'],
    link_args : ['-Wl,--gc-sections'],
    install : false,
  )
  test('CaptionPgsRenderTests', caption_pgs_render_tests,
    suite : 'native',
    timeout : 60,
  )
endif
