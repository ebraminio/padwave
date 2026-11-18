project(
  'dualsense-demo',
  'cpp',
  meson_version : '>= 1.3.0',
  version : '0.0.1',
  default_options: [
    'warning_level=3',
    'cpp_eh=none',
    'cpp_rtti=false',
    'cpp_std=c++23',
    'default_library=static',
  ]
)

hidapi_dep = dependency('hidapi', required : false, allow_fallback : false)
if not hidapi_dep.found()
  hidapi_dep = dependency('hidapi-hidraw', required : false, allow_fallback : false)
endif
if not hidapi_dep.found()
  hidapi_dep = dependency('hidapi', fallback : ['hidapi', 'hidapi_dep'])
endif
opus_dep = dependency('opus', fallback : ['opus', 'opus_dep'])

demo_args = ['-DDS_HAVE_OPUS']
if host_machine.system() == 'windows'
  demo_args += ['-D_CRT_SECURE_NO_WARNINGS']
endif

exe = executable(
  'dualsense-demo',
  ['../dualsense-demo.cc'],
  cpp_args : demo_args,
  dependencies : [hidapi_dep, opus_dep],
)

test('dualsense-demo-help', exe, args : ['--help'], timeout : 10)
