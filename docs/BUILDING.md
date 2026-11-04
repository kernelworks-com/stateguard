# Build, install, and CI

Requires CMake 3.20 or newer and a C++20 compiler. The CPU examples need no GPU,
model downloads, account, or third-party runtime libraries.

Local checks have passed with Apple Clang 17, GCC 13, and Clang 18.

## Check your tools

```sh
cmake --version
c++ --version
```

On macOS, if CMake is missing and you use Homebrew:

```sh
brew install cmake
```

If the Apple compiler is missing, install the Command Line Tools with
`xcode-select --install`, then follow the installer before building.

On Ubuntu 24.04, install the build tools if needed:

```sh
sudo apt-get update
sudo apt-get install cmake g++ make
```

## Build from the repository root

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --no-tests=error
```

## macOS: standard C++ headers not found

If compilation fails on a header such as `cstdint`, your compiler may not be
finding the SDK's standard library headers. Reconfigure with this single-line
command, then rerun the build and tests above:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-isystem$(xcrun --show-sdk-path)/usr/include/c++/v1"
```

If your terminal shows `dquote>`, a pasted double quote was left unclosed.
Press Ctrl+C and paste the complete single-line command again.

## Optional CUDA harness

The CUDA copy demo needs an NVIDIA GPU and the CUDA toolkit; it is off by default.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSTATEGUARD_ENABLE_CUDA=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --no-tests=error
```

If CMake cannot find `nvcc`, add `-DCUDAToolkit_ROOT=/usr/local/cuda` (or your
toolkit path). To run the whole check on a clean single-GPU machine such as Google
Colab, run `bash scripts/colab_gpu_check.sh <commit>` from a clone of this
repository.

## Install and uninstall

Stateguard 0.1.1 is experimental: no ABI promise and no package-manager
integration. The GitHub Releases page carries unsigned CLI bundles for Linux
x86_64, Linux arm64, and macOS arm64 plus a checksummed source archive as a
convenience; building from source is the supported path. Install into a prefix
of your own:

```sh
cmake --install build --prefix "$HOME/.local/stateguard"
export PATH="$HOME/.local/stateguard/bin:$PATH"
stateguard-timeline "$HOME/.local/stateguard/share/stateguard/traces/cancellation_timeline.sgtrace"
```

That prints `complete` and `cancellation_to_reuse 5500 ns` (a synthetic clock).
The install has the core library, public headers, the four CLI tools, the
synthetic sample traces, and the license; with examples enabled (the default)
also the instrumented, async-copy, and cancellation demos. Tests, harness
libraries, CUDA binaries, and the Python adapter are not installed. Configure
with `-DSTATEGUARD_BUILD_TESTS=OFF -DSTATEGUARD_BUILD_EXAMPLES=OFF` for a smaller
install. The library is static by default; `-DBUILD_SHARED_LIBS=ON` builds a
shared one, and installed binaries find it through a relative runtime path.

Another CMake project uses the installed package with
`-DCMAKE_PREFIX_PATH="$HOME/.local/stateguard"`:

```cmake
find_package(stateguard 0.1.1 EXACT CONFIG REQUIRED)
target_link_libraries(example PRIVATE stateguard::stateguard)
```

The version must match exactly during the alpha.

To uninstall, keep the build directory: it records installed files in
`install_manifest.txt`, and this removes exactly those files (unrelated files and
empty directories stay):

```sh
cmake --build build --target uninstall
```

Use one build directory per prefix, because a later install from the same build
replaces the manifest. `DESTDIR` works if it is set for the uninstall too.

`scripts/check_install.py` (Python 3.9+, standard library) checks all of this
in a fresh directory: it builds, installs, moves the prefix (to a path with a
space), compiles a separate consumer, runs the tools and demos, and uninstalls.
Add `--shared` for the shared library; on macOS, pass the SDK flag above with
`--cxx-flags`.

```sh
python3 scripts/check_install.py --workdir build-install-check
```

## Continuous integration

Every pull request and push to `main` runs:

| Runner | Compiler | Configuration |
|---|---|---|
| Ubuntu 24.04 | GCC 13 | Release, warnings as errors, install checks |
| Ubuntu 24.04 | Clang 18 | Debug, address and undefined-behavior sanitizers |
| macOS 14 | Apple Clang | Release, warnings as errors, install checks |

No GPU or serving engine is tested. Actions are pinned to commits, and checkout
credentials are not kept. After the tests pass on `main`, the workflow archives
the committed source, builds, tests, and installs the extracted archive, and
uploads `source.tar.gz` with `SHA256SUMS` for 14 days. That is a tested source
bundle, not a release. To reproduce the GCC job:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-13 -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wpedantic -Werror"
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure --no-tests=error
```

For the sanitizer job use `clang++-18`, `Debug`, and add
`-fsanitize=address,undefined -fno-omit-frame-pointer` to the flags.

## Report a build issue

Include your operating system, compiler and CMake versions, the command you ran,
and its error output. Remove private paths, credentials, and customer data before
sharing logs.
