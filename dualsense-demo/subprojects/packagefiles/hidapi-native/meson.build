project('hidapi', 'c', meson_version : '>=0.57.0', version : files('VERSION'))

hidapi_inc = [include_directories('.', 'hidapi')]
hidapi_sources = []
hidapi_deps = [dependency('threads')]
hidapi_c_args = []

if host_machine.system() == 'windows'
  hidapi_inc += [include_directories('windows')]
  hidapi_sources += files('windows/hid.c', 'windows/hidapi_descriptor_reconstruct.c')
  hidapi_c_args += ['-DHIDAPI_CMAKE_V0_BUILD', '-DHID_API_NO_EXPORT_DEFINE']
elif host_machine.system() == 'darwin'
  hidapi_inc += [include_directories('mac')]
  hidapi_sources += files('mac/hid.c')
  hidapi_deps += dependency('appleframeworks', modules : ['IOKit', 'CoreFoundation'])
elif host_machine.system() == 'linux'
  hidapi_inc += [include_directories('linux')]
  hidapi_sources += files('linux/hid.c')
  hidapi_c_args += ['-D_GNU_SOURCE']
  hidapi_deps += dependency('libudev')
else
  error('hidapi native Meson build supports Windows, macOS and Linux')
endif

hidapi_lib = static_library(
  'hidapi',
  hidapi_sources,
  include_directories : hidapi_inc,
  dependencies : hidapi_deps,
  c_args : hidapi_c_args,
)

hidapi_dep = declare_dependency(
  link_with : hidapi_lib,
  include_directories : include_directories('hidapi'),
  dependencies : hidapi_deps,
)
