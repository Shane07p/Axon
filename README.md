# Axon

Parallel, resumable file transfer over TCP, in C++20.

A nerve is a bundle of axons carrying signals side by side. Axon bundles
several TCP connections into one download, and if one is cut the others keep
going and the transfer resumes from the exact byte.

> Status: early setup. Only `axon --version` works so far.

## Build

Requires Linux (or WSL), CMake 3.25+ and a C++20 compiler.

On WSL with the source under `/mnt/c`, put the build directory on the Linux
filesystem (e.g. `-B ~/axon-build/debug`): it is much faster, and Windows
Defender scanning `/mnt/c` can make the GoogleTest download step fail.

```sh
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/axon --version
```

Sanitizer build (AddressSanitizer + UndefinedBehaviorSanitizer):

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DAXON_SANITIZE=address,undefined
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
```

## Layout

```
apps/axon/       CLI entry point
include/axon/    public headers of the core library
src/             core library (axon_core)
tests/unit/      GoogleTest unit tests
```
