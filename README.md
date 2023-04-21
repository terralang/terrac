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

You need CMake, zLib (libz), libcurses, and Terra.

Building with CMake should be straightforward, but if you're new:

```console
$ mkdir build && cd build
$ cmake .. -DCMAKE_BUILD_TYPE=Debug -DTERRA_PREFIX=/path/to/terra
$ make
```

The `TERRA_PREFIX` path should be an absolute path to the root of an unpacked Terra release -
i.e., the path `${TERRA_PREFIX}/{lib,include,bin}` should all exist.

# License
Terra is licensed under the [MIT License](LICENSE)

Originally by Josh Junon ([@qix-](https://github.com/qix-)).
