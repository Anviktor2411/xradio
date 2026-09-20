#!/bin/sh
# Cut a release: asks for the version number, runs the checks, writes the zip
# into dist/. Run it with `bash release.sh` -- uploading through the GitHub
# web interface strips the executable bit, so it may not be runnable directly.
cd "$(dirname "$0")" || exit 1
exec python3 tools/release.py "$@"
