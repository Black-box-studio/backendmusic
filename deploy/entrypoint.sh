#!/bin/sh
set -eu
umask 077
exec /opt/glass/glass-music-server --host 0.0.0.0 --port 8787 --db /data/music.sqlite3 --assets /opt/glass/assets
