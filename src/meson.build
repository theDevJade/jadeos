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

# wasm_bindings.cpp requires Emscripten headers
if host_machine.system() == 'emscripten'
  cpu_sources += files('bindings/wasm_bindings.cpp')
endif

executable('jadeportfolio',
  cpu_sources,
  include_directories : include_directories('.'),
  cpp_args  : (host_machine.system() == 'emscripten' ? web_flags       : native_flags),
  link_args : (host_machine.system() == 'emscripten' ? web_link_flags  : []),
  install : true,
)
