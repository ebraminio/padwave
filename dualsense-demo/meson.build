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
    'wrap_mode=forcefallback',
  ]
)

hidapi_dep = dependency('hidapi')
opus_dep = dependency('opus')

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
