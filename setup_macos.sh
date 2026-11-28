#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
if [[ "$(uname -s)" != Darwin ]]; then
  echo 'macOS에서 실행하세요.' >&2
  exit 1
fi
python_bin="${AAANIMAL_PYTHON:-python3.11}"
if ! command -v "$python_bin" >/dev/null; then
  if command -v brew >/dev/null && [[ -x "$(brew --prefix python@3.11)/bin/python3.11" ]]; then
    python_bin="$(brew --prefix python@3.11)/bin/python3.11"
  else
    echo 'Python 3.11이 필요합니다: brew install python@3.11' >&2
    exit 1
  fi
fi
"$python_bin" -c 'import sys; assert sys.version_info[:2] == (3, 11), "Python 3.11을 사용하세요."'
"$python_bin" -m venv .venv
.venv/bin/python -m pip install --upgrade pip
.venv/bin/python -m pip install -r ai/requirements.txt
.venv/bin/python -m pip check
.venv/bin/python tools/check_runtime.py
bash build_macos.sh
echo '준비 완료. ./run.command 로 실행하세요.'
