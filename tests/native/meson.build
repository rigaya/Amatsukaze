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

trim_adjust_audio_tests = executable('TrimAdjustAudioTests',
  'TrimAdjustAudioTests.cpp',
  install : false,
)
test('TrimAdjustAudioTests', trim_adjust_audio_tests,
  suite : 'native',
  timeout : 30,
)

logo_gop_decoder_tests = executable('LogoGopDecoderTests',
  'LogoGopDecoderTests.cpp',
  dependencies : ffmpeg_dep + [dependency('threads')],
  install : false,
)
test('LogoGopDecoderTests', logo_gop_decoder_tests,
  suite : 'native',
  timeout : 60,
)

av_frame_pool_tests = executable('AVFramePoolTests',
  'AVFramePoolTests.cpp',
  dependencies : ffmpeg_dep + [common_dep, dependency('threads')],
  install : false,
)
test('AVFramePoolTests', av_frame_pool_tests,
  suite : 'native',
  timeout : 30,
)

logo_decode_fallback_tests = executable('LogoDecodeFallbackTests',
  'LogoDecodeFallbackTests.cpp',
  dependencies : [amatsukaze_dep] + amatsukaze_deps,
  cpp_args : host_machine.system() == 'windows' ? ['-DAMATSUKAZE_IMPORTS'] : [],
  install : false,
)
test('LogoDecodeFallbackTests', logo_decode_fallback_tests,
  suite : 'native',
  timeout : 120,
)

# ロゴ枠検出の左側弱輪郭救済の判定方針 (ヘッダーのみの純粋関数) を単体で確認する。
logo_scan_weak_side_rescue_policy_tests = executable('LogoScanWeakSideRescuePolicyTests',
  'LogoScanWeakSideRescuePolicyTests.cpp',
  include_directories : include_directories('../../Amatsukaze'),
  install : false,
)
test('LogoScanWeakSideRescuePolicyTests', logo_scan_weak_side_rescue_policy_tests,
  suite : 'native',
  timeout : 30,
)

# ロゴ生成の品質判定 (no_logo) の境界を確認する。
logo_scan_quality_tests = executable('LogoScanQualityTests',
  'LogoScanQualityTests.cpp',
  include_directories : [include_directories('../../Amatsukaze'), amatsukaze_include_dirs],
  dependencies : amatsukaze_deps,
  install : false,
)
test('LogoScanQualityTests', logo_scan_quality_tests,
  suite : 'native',
  timeout : 30,
)
