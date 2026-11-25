"""Install this application's dependencies on first launch or after an update."""

import hashlib
from pathlib import Path
import subprocess
import sys


def main():
    requirements = Path(__file__).with_name("requirements.txt")
    digest = hashlib.sha256(requirements.read_bytes()).hexdigest()
    marker = Path(sys.prefix) / ".fauna-dependencies"
    if marker.exists() and marker.read_text() == digest:
        return
    subprocess.run(
        [sys.executable, "-m", "pip", "install", "--disable-pip-version-check",
         "-r", str(requirements)], check=True
    )
    marker.write_text(digest)


if __name__ == "__main__":
    main()
