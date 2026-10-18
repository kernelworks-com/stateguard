# Build help

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

## Report a build issue

Include your operating system, compiler and CMake versions, the command you ran,
and its error output. Remove private paths, credentials, and customer data before
sharing logs. For the automated build matrix, see [CI details](../.github/CI.md).
