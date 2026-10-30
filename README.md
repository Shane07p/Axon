# Axon

Parallel, resumable file transfer over TCP, in C++20.

A nerve is a bundle of axons carrying signals side by side. Axon bundles
several TCP connections into one download, and if one is cut the others keep
going and the transfer resumes from the exact byte.

> Status: work in progress. The server (`axon serve`) works; the client
> (`axon fetch`, `axon list`) comes next.

## Run

A dataset is a folder of topics; each topic is a sub-folder of files:

```
datasets/
├── animals/   cat.jpg  dog.jpg
└── plants/    fern.txt
```

```sh
axon serve --root ./datasets --port 9000
```

Ctrl+C stops the server cleanly (a second Ctrl+C forces it). Symlinks,
nested folders and unreadable files are skipped with a warning.

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
src/net/         sockets
src/protocol/    message encoding and framing
src/storage/     files and the dataset scanner
src/transfer/    server (client to come)
tests/unit/      GoogleTest unit tests
```
