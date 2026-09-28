# Glass Music backend

C++17/Drogon API for the Glass Music Qt application. It provides accounts, administrator roles, catalog search, playlists, likes, profiles, welcome slides, theme settings, audio streaming, and named reusable storage connections.

Storage adapters support local files, MongoDB GridFS, Firebase Cloud Storage, Google Drive and S3-compatible services. SQLite stores the account/catalog index; provider credentials are AES-256-GCM encrypted with a separate private storage key. New catalogs and playlists start empty. Bundled synthetic WAV files are integration-test fixtures, not automatically published songs.

## GitHub-only testing

Use [GitHub Codespaces](deploy/CODESPACES.md) for a temporary backend endpoint. It stops with the development environment and is subject to included usage limits; it is not permanent free hosting.

## Deploy

This repository contains backend code, not a hosted server. GitHub Actions builds/tests the container; it does not publish app releases. Deploy the Docker image to a container host or VPS with HTTPS and persistent storage. See [deployment instructions](deploy/README.md).

```sh
docker compose -f deploy/compose.yml up -d --build
```

The API listens on port 8787 inside the container. Compose exposes it only on the host's loopback interface for a TLS reverse proxy. Preserve the `music-data` volume, including `music.sqlite3` and `storage.key`, across deployments. Configure the app with the resulting HTTPS origin.

After the first server startup, create your administrator interactively:

```sh
docker compose -f deploy/compose.yml exec api python3 /opt/glass/set-admin-password.py admin@gmail.com --db /data/music.sqlite3
```

No default password, database, cloud credentials or signing keys are included. The local development administrator is not automatically copied to a hosted deployment. Configure each real provider through **Admin studio → Storage** after signing in.

## Native development

Requirements: CMake 3.21+, C++17, Drogon, Qt Core 6.5+, OpenSSL 3, SQLite3 and Python 3 with venv support. The Docker build uses Ubuntu 26.04.

```sh
python3 -m venv .runtime/storage-env
.runtime/storage-env/bin/pip install -r provider/storage-requirements.txt
cmake -S server -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
mkdir -p .runtime
export GLASS_STORAGE_PYTHON="$PWD/.runtime/storage-env/bin/python"
./build/glass-music-server --db .runtime/music.sqlite3 --assets assets
```

Test with an isolated temporary database:

```sh
GLASS_STORAGE_PYTHON="$PWD/.runtime/storage-env/bin/python" \
  python3 tests/api_integration.py --server build/glass-music-server
```

The tests cover auth/roles, encrypted reusable storage connections, uploads to two independent local stores, duration extraction, catalog synchronization, authenticated range streaming, session revocation, playlist pins, profiles, welcome editing and restart persistence. Live cloud uploads require administrator-supplied credentials and remain a deployment verification step.

## Scope and release status

- Upload limit: 32 MiB. Provider scans import app-managed objects, bounded to 1,000 per connection.
- The native YouTube provider currently runs on the desktop client; this backend does not yet supply it to Android.
- Use a single API replica with the SQLite volume. Configure cache retention, backups and operational monitoring before broader use.
- The Android test candidate remains local. APK/desktop publication is on hold until the hosted API and real client flows are verified.
