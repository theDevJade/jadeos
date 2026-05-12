cpu_sources = files(
  'cpu/cpu.cpp',
  'cpu/memory.cpp',
  'gpu/gpu.cpp',
  'gpu/font.cpp',
  'gpu/rasterizer.cpp',
  'os/kernel.cpp',
  'os/scheduler.cpp',
  'os/filesystem.cpp',
  'os/wm.cpp',
  'os/terminal.cpp',
  'os/apps/clock.cpp',
  'os/apps/calculator.cpp',
  'os/apps/fileexplorer.cpp',
  'os/apps/taskmanager.cpp',
  'os/apps/wasminfo.cpp',
  'main.cpp',
)

_inc_dirs = [include_directories('.')]
doom_link = []

if host_machine.system() == 'emscripten'
  fs_mod = import('fs')
  _doom_list = fs_mod.read(meson.current_source_dir() / '../third_party/doomgeneric/doom_sources.list').split('\n')
  doom_srcs = []
  foreach _ln : _doom_list
    _line = _ln.strip()
    if _line == ''
      continue
    endif
    doom_srcs += files('../third_party/doomgeneric/doomgeneric' / _line)
  endforeach

  doom_inc = include_directories('../third_party/doomgeneric/doomgeneric')
  opl_inc  = include_directories('../third_party/opl')
  opl_srcs = files(
    '../third_party/opl/opl.c',
    '../third_party/opl/opl3.c',
    '../third_party/opl/opl_queue.c',
    '../third_party/opl/opl_jade.c',
  )

  doom_lib = static_library(
    'doomgeneric',
    doom_srcs + opl_srcs,
    include_directories : [doom_inc, opl_inc],
    c_args : [
      '-std=gnu11',
      '-DFEATURE_SOUND',
      '-D_DEFAULT_SOURCE',
      '-fno-common',
      '-Wno-unused-parameter',
      '-Wno-unused',
      '-Wno-sign-compare',
      '-Wno-missing-field-initializers',
      '-Wno-strict-prototypes',
      '-Wno-unused-but-set-variable',
      '-Wno-type-limits',
      '-DHAVE_STDINT_H=1',
      '-DHAVE_STDLIB_H=1',
      '-DHAVE_STRING_H=1',
      '-DHAVE_STRINGS_H=1',
      '-DHAVE_SYS_TYPES_H=1',
    ],
  )

  doom_link += declare_dependency(
    link_with : doom_lib,
    include_directories : [doom_inc, opl_inc],
  )

  stb_inc = include_directories('../third_party/stb')

  doom_link += declare_dependency(
    include_directories : stb_inc,
  )

  cpu_sources += files('os/apps/doom_port.cpp', 'os/apps/media_player.cpp', 'audio/jade_audio.cpp')
  _inc_dirs += doom_inc
endif

# wasm_bindings.cpp requires Emscripten headers
if host_machine.system() == 'emscripten'
  cpu_sources += files('bindings/wasm_bindings.cpp')
endif

executable('jadeportfolio',
  cpu_sources,
  include_directories : _inc_dirs,
  dependencies : doom_link,
  cpp_args  : (host_machine.system() == 'emscripten' ? web_flags       : native_flags),
  link_args : (host_machine.system() == 'emscripten' ? web_link_flags  : []),
  install : true,
)
