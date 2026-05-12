project('jadeportfolio', 'cpp',
  version : '0.1.0',
  default_options : [
    'cpp_std=c++20',
    'warning_level=3',
    'optimization=2',
  ]
)

cpp = meson.get_compiler('cpp')

# Flags only valid at link time (Emscripten -s settings, --bind).
web_link_flags = []
# Flags valid at both compile and link time.
web_flags = []
native_flags = []

if host_machine.system() == 'emscripten'
  web_link_flags += [
    '-sALLOW_MEMORY_GROWTH=1',
    '-sINITIAL_MEMORY=268435456',   # 256 MiB
    '-sMAXIMUM_MEMORY=1073741824',  # 1 GiB cap
    '-sEXPORTED_RUNTIME_METHODS=["ccall","cwrap","HEAPU8","HEAPU32"]',
    '-sEXPORT_ES6=1',
    '-sMODULARIZE=1',
    '-sEXPORT_NAME=JadePortfolioModule',
    '-sWASM=1',
    '-sENVIRONMENT=web',
    '-sNO_EXIT_RUNTIME=1',
    '--bind',
  ]
else
  native_flags += ['-march=native']
endif

subdir('src')
subdir('web')
