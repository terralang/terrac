project('terrac', 'cpp', default_options: ['cpp_std=c++11'])

xopt = subproject('xopt').get_variable('dep')
filesystem = subproject('filesystem').get_variable('dep')
zlib = dependency('zlib')
curses = dependency('ncurses')
libterra = meson.get_compiler('c').find_library('terra')
threads = dependency('threads')
dl = meson.get_compiler('c').find_library('dl', required: false)

exe = executable(
  'terrac',
  'main.cc',
  dependencies: [
    xopt,
    filesystem,
    zlib,
    curses,
    libterra,
    threads,
    dl
  ],
  cpp_args: ['-Wall', '-Wextra', '-Werror'],
  link_args: ['-pagezero_size', '10000', '-image_base', '100000000'])
