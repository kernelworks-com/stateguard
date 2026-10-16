#!/usr/bin/env bash
# Builds Stateguard with the optional CUDA harness on a machine with one NVIDIA
# GPU (for example a Google Colab GPU runtime), runs the test suite and the
# CUDA copy demo, and prints an experiment record.
#
# usage: colab_gpu_check.sh REF [REPOSITORY_URL]
#
# REF is a branch, tag, or full commit hash. Every step runs under a timeout,
# and no process is left running when the script exits. The work directory
# (STATEGUARD_WORK, default /tmp/stateguard-gpu-check) is replaced on each run.
# Exits 0 only when every step succeeded.

set -uo pipefail

ref=${1:?usage: colab_gpu_check.sh REF [REPOSITORY_URL]}
repository=${2:-https://github.com/kernelworks-com/stateguard.git}
work=${STATEGUARD_WORK:-/tmp/stateguard-gpu-check}
demo_repeat=${STATEGUARD_DEMO_REPEAT:-20}

failures=()
step() {
  local name=$1 limit=$2
  shift 2
  echo "== ${name}"
  timeout --kill-after=10 "${limit}" "$@"
  local status=$?
  if [ "${status}" -ne 0 ]; then
    [ "${status}" -eq 124 ] && echo "timed out after ${limit}s"
    failures+=("${name} (exit ${status})")
  fi
  return "${status}"
}

rm -rf "${work}"
mkdir -p "${work}"
log=${work}/log
mkdir -p "${log}"

step clone 180 git clone --quiet --no-checkout "${repository}" "${work}/src" &&
  step fetch 180 git -C "${work}/src" fetch --quiet --depth 1 origin "${ref}" &&
  step checkout 60 git -C "${work}/src" checkout --quiet --detach FETCH_HEAD

cmake_bin=cmake
if ! cmake --version 2>/dev/null | head -n1 |
  awk '{ split($3, v, "."); exit !(v[1] > 3 || (v[1] == 3 && v[2] >= 20)) }'; then
  step "install cmake" 300 python3 -m venv "${work}/venv" &&
    step "install cmake" 300 "${work}/venv/bin/pip" install --quiet 'cmake>=3.20,<4' &&
    cmake_bin=${work}/venv/bin/cmake
fi

cuda_root=()
[ -d /usr/local/cuda ] && cuda_root=(-DCUDAToolkit_ROOT=/usr/local/cuda)

if [ ${#failures[@]} -eq 0 ]; then
  step configure 300 "${cmake_bin}" -S "${work}/src" -B "${work}/build" \
    -DCMAKE_BUILD_TYPE=Release -DSTATEGUARD_ENABLE_CUDA=ON \
    -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wpedantic" "${cuda_root[@]}" \
    >"${log}/configure.txt" 2>&1 || tail -n 30 "${log}/configure.txt"
fi
if [ ${#failures[@]} -eq 0 ]; then
  step build 900 "${cmake_bin}" --build "${work}/build" --parallel 2 \
    >"${log}/build.txt" 2>&1 || tail -n 40 "${log}/build.txt"
fi
if [ ${#failures[@]} -eq 0 ]; then
  ctest_bin=$(dirname "$(command -v "${cmake_bin}")")/ctest
  step ctest 900 "${ctest_bin}" --test-dir "${work}/build" --output-on-failure \
    --no-tests=error --timeout 300 >"${log}/ctest.txt" 2>&1
  tail -n 30 "${log}/ctest.txt"
  step demo 600 "${work}/build/stateguard-cuda-copy-demo" --repeat "${demo_repeat}" \
    >"${log}/demo.txt" 2>&1
  cat "${log}/demo.txt"
fi

echo
echo "===== Stateguard GPU experiment record ====="
echo "date (UTC):  $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "repository:  ${repository}"
echo "ref:         ${ref}"
echo "commit:      $(git -C "${work}/src" rev-parse HEAD 2>/dev/null || echo unknown)"
echo "os:          $(. /etc/os-release 2>/dev/null && echo "${PRETTY_NAME}" || uname -sr)"
echo "gpu:         $(nvidia-smi --query-gpu=name,driver_version,memory.total \
  --format=csv,noheader 2>/dev/null || echo 'nvidia-smi unavailable')"
echo "nvcc:        $( (nvcc --version 2>/dev/null || /usr/local/cuda/bin/nvcc --version 2>/dev/null) |
  grep -o 'release [0-9.]*' || echo unavailable)"
echo "compiler:    $(c++ --version 2>/dev/null | head -n1)"
echo "cmake:       $("${cmake_bin}" --version 2>/dev/null | head -n1)"
echo "warnings:    $(grep -c 'warning:' "${log}/build.txt" 2>/dev/null || true)"
echo "ctest:       $(grep -E 'tests passed|tests failed' "${log}/ctest.txt" 2>/dev/null || echo 'not run')"
echo "demo repeat: ${demo_repeat}"
if [ ${#failures[@]} -eq 0 ]; then
  echo "result:      all steps succeeded"
else
  echo "result:      failed: ${failures[*]}"
fi
echo "logs:        ${log}"
[ ${#failures[@]} -eq 0 ]
