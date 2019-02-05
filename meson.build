project('terrac', 'cpp', default_options: ['cpp_std=c++11'])

xopt = subproject('xopt').get_variable('dep')
filesystem = subproject('filesystem').get_variable('dep')
zlib = dependency('zlib')
curses = dependency('curses')
libterra = meson.get_compiler('c').find_library('terra')

executable(
  'terrac',
  'main.cc',
  dependencies: [
    xopt,
    filesystem,
    zlib,
    curses,
    libterra
  ])
