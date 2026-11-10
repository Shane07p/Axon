# Axon

Parallel, resumable file transfer over TCP, in C++20.

A nerve is a bundle of axons carrying signals side by side. Axon bundles
several TCP connections into one download, and if one is cut the others keep
going and the transfer resumes from the exact byte.

> Status: work in progress. `axon serve`, `axon list` and `axon fetch` work;
> a download can use several connections and survives dropped ones, server
> restarts and client restarts. Next: detecting files changed during a transfer.

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

`--connections N` shares the download over N connections, which take files
from a common queue a batch at a time. On loopback (8 cores) 400 files of
100 KB went from 157 MB/s with one connection to 541 MB/s with eight.

If a connection drops, its unfinished files go back to the queue (another
connection may take them over at once), and it reconnects with backoff and
rejoins the session; every file continues from the exact byte it has. To see
it, make the server cut connections on purpose:

```sh
axon serve --root ./datasets --fail-prob 0.7 --seed 1   # cut 70% of connections
axon fetch localhost:9000 animals --output ./downloads --connections 4
# axon: connection closed after <n> of <m> bytes; reconnecting in <t> ms
# ...
# fetched animals: ... into downloads/animals, <k> reconnect(s)
```

Rerun the same `axon fetch` command after an interruption to resume. While
downloading, Axon saves progress about once a second in
`downloads/animals/.axon-state`; a crash may require downloading the bytes
since the last checkpoint again. If a fetch gives up, it saves the exact
progress before reporting the error. Files whose name, size or CRC-32 changed
on the server start over. The checkpoint is removed when the download succeeds.
The file names `.axon-state` and `.axon-state.tmp` are reserved for this metadata.

For a client restart, interrupt a large fetch with Ctrl+C after a few seconds,
then rerun it with the same topic and output folder:

```sh
axon fetch localhost:9000 animals --output ./downloads
# Ctrl+C partway through, then:
axon fetch localhost:9000 animals --output ./downloads
# fetched animals: ... into downloads/animals, <size> kept from before
```

For a server restart, leave the fetch running in its terminal. In the server's
terminal, press Ctrl+C and promptly start it again on the same port:

```sh
axon serve --root ./datasets --port 9000
```

The client reconnects automatically and keeps every received byte of unchanged
files. Its final summary reports `<size> kept from before`. Restart promptly:
the default retry budget allows roughly five seconds without progress.

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
It also kills a client with SIGKILL and reruns it, and restarts a server during
a fetch; both must preserve progress and produce identical files.

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
src/transfer/    server (one poll() loop per session) and client
tests/unit/      GoogleTest unit tests
```
