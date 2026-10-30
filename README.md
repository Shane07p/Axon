# Axon

Parallel, resumable file transfer over TCP, in C++20.

A nerve is a bundle of axons carrying signals side by side. Axon bundles
several TCP connections into one download, and if one is cut the others keep
going and the transfer resumes from the exact byte.

> Status: work in progress. `axon serve`, `axon list` and `axon fetch` work
> over one connection; resume after a dropped connection and parallel
> connections come next.

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

```sh
axon list localhost:9000
# TOPIC     FILES       SIZE
# animals       2     <size>
# plants        1     <size>

axon fetch localhost:9000 animals --output ./downloads
# fetched animals: <files>, <size> in <seconds> (<MB/s>) into downloads/animals
```

Each file is downloaded to a hidden temporary file, checked against the CRC-32
the server announced, and only then renamed to its real name, so a file you
see is always complete. Other files in the folder are left alone.

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

`ctest` runs the GoogleTest unit tests and an end-to-end test
(`tests/integration/serve_fetch.sh`) that starts the real `axon serve`, runs
`axon list` and `axon fetch` against it, compares the files with `diff -r`,
checks the exit codes of error cases and stops the server with Ctrl+C.

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
src/transfer/    server and client
tests/unit/      GoogleTest unit tests
```
