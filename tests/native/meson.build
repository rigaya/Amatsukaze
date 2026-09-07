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
