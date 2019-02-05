# terrac

The unofficial [Terra](http://terralang.org/) compiler.

## Usage

```console
$ terrac --help

usage: terrac terrac [-h] [--] file.t

Unofficial Terra compiler

-o, --output           If specified, outputs the terra code to the given filename
-I, --include-dir=dir  Adds a search path for C header files (can be passed multiple times)
-L, --lib-dir=dir      Adds a search path for libraries (can be passed multiple times)
-l, --lib=name         Specifies a library to be linked against the resulting binary
-D, --depfile          If specified, emits a Ninja-compatible depfile with all included files during the build
-P, --depfile-path     If specified, all depfile paths are relativized to this path
-v                     Increase verbosity (default level 0, max level 3)
-g, --debug            Enable debugging information
-h, --help             Shows this help message
```

## Building

You need Meson, zLib (libz), libcurses, and Terra.

Further, clone recursively (or run `git submodule update --init --recursive`) or add
as a submodule, making sure `terrac`'s sources are under `subprojects/terrac`.

In your `meson.build`:
```meson
# XXX Proper generator can't be made by us _yet_ since there is a bug
#     affecting path resolutions from within subprojects:
#
#     https://github.com/mesonbuild/meson/issues/4880
#
#     This means the code below is, well, untested and not official :c

terrac_exe = subproject('terrac').get_variable('exe')
terrac = generator(
           terrac_exe,
           depfile: '@PLAINNAME@.dep',
           output: '@PLAINNAME@.o',
           arguments: ['-o', '@OUTPUT@', '-D', '@DEPFILE@', '-P', '@BUILD_DIR@', '@INPUT@'])

executable('my-app', terrac.process('app.t'))
```

Optionally, you can build with CMake, though this is considered deprecated.
Building with CMake should be straightforward, but if you're new:

```console
$ mkdir build && cd build
$ cmake .. -DCMAKE_BUILD_TYPE=Debug
$ make
```

# License
Terrac is [Unlicensed](http://unlicense.org/).

Terra is released under the MIT license.

Originally by Sir Toastie ([@qix-](https://github.com/qix-)) for the development of [Tide](https://reddit.com/r/tidemmo).
