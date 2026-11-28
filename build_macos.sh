#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
if [[ "$(uname -s)" != Darwin ]]; then
  echo '이 실행 스크립트는 macOS용입니다.' >&2
  exit 1
fi
xcrun --find clang >/dev/null
if ! command -v cmake >/dev/null; then
  echo 'CMake가 필요합니다: brew install cmake' >&2
  exit 1
fi
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0
cmake --build build --parallel
ctest --test-dir build --output-on-failure
