#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
umask 077
mkdir -p "$project_dir/.runtime"
if [[ ! -x "$project_dir/build-codespace/glass-music-server" ]]; then
    echo 'Build the backend first: cmake -S server -B build-codespace && cmake --build build-codespace' >&2
    exit 1
fi
# The lock prevents duplicate servers when reconnecting to a running codespace.
nohup flock -n "$project_dir/.runtime/codespace.lock" \
    "$project_dir/build-codespace/glass-music-server" \
    --host 0.0.0.0 --port 8787 --db "$project_dir/.runtime/music.sqlite3" \
    --assets "$project_dir/assets" >> "$project_dir/.runtime/codespace.log" 2>&1 < /dev/null &
python3 - <<'PY'
import time, urllib.request
for attempt in range(40):
    try:
        with urllib.request.urlopen('http://127.0.0.1:8787/api/v1/health', timeout=1) as response:
            if response.status == 200:
                print('Glass Music test API is ready on port 8787.')
                break
    except OSError:
        time.sleep(.25)
else:
    raise SystemExit('API did not start; inspect .runtime/codespace.log.')
PY
