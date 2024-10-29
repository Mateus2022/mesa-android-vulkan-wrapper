wrapper_icd = custom_target(
  'wrapper_icd',
  input: [vk_icd_gen, vk_api_xml],
  output: 'wrapper_icd.@0@.json'.format(host_machine.cpu()),
  command: [
    prog_python,
    '@INPUT0@',
    '--api-version', '1.3',
    '--xml', '@INPUT1@',
    '--lib-path', join_paths(get_option('prefix'), get_option('libdir'), 'libvulkan_wrapper.so'),
    '--out', '@OUTPUT@',
  ],
  build_by_default: true,
  install_dir: with_vulkan_icd_dir,
  install: true,
)

wrapper_entrypoints = custom_target(
  'wrapper_entrypoints',
  input: [vk_entrypoints_gen, vk_api_xml],
  output: ['wrapper_entrypoints.h', 'wrapper_entrypoints.c'],
  command: [
    prog_python,
    '@INPUT0@',
    '--xml', '@INPUT1@',
    '--proto',
    '--weak',
    '--beta', with_vulkan_beta.to_string(),
    '--out-h', '@OUTPUT0@',
    '--out-c', '@OUTPUT1@',
    '--prefix', 'wrapper',
  ],
  depend_files: vk_entrypoints_gen_depend_files,
)

vk_wrapper_trampolines_gen = files('vk_wrapper_trampolines_gen.py')

vk_wrapper_trampolines = custom_target(
  'wrapper_trampolines',
  input: [vk_wrapper_trampolines_gen, vk_api_xml],
  output: ['wrapper_trampolines.c', 'wrapper_trampolines.h'],
  command: [
    prog_python,
    '@INPUT0@',
    '--xml', '@INPUT1@',
    '--out-c', '@OUTPUT0@',
    '--out-h', '@OUTPUT1@',
    '--beta', with_vulkan_beta.to_string(),
  ],
  depend_files: vk_dispatch_trampolines_gen_depend_files,
)

wrapper_files = files(
  'wrapper_instance.c',
  'wrapper_device.c',
  'wrapper_physical_device.c'
)

wrapper_deps = [
  idep_vulkan_runtime,
  idep_vulkan_util,
  idep_vulkan_wsi,
]

libvulkan_wrapper = shared_library(
  'vulkan_wrapper',
  [wrapper_entrypoints, vk_wrapper_trampolines, wrapper_files],
  include_directories: [
    inc_include,
    inc_src,
  ],
  dependencies: [wrapper_deps, vulkan_wsi_deps],
  gnu_symbol_visibility: 'hidden',
  install: true,
)
