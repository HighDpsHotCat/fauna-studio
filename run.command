#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
bash build_macos.sh
export AAANIMAL_ROOT="$PWD"
export PYTORCH_ENABLE_MPS_FALLBACK=1
exec './build/AAAnimal Studio.app/Contents/MacOS/AAAnimal Studio'
