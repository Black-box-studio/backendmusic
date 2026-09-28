# Hosted backend before application releases

GitHub stores this source and runs `.github/workflows/backend-check.yml`. GitHub Pages cannot run the C++/Drogon backend. A separate container host or VPS with HTTPS and persistent disk is required. No account, repository, host, domain or deployment is created by these files, and the workflow does not publish images or application releases.

The Dockerfile and Compose configuration are prepared but have not been container-built locally: Docker is unavailable on this machine. The native API integration tests passed. Run the workflow or `docker build` before deployment.

## VPS/container setup

Clone `https://github.com/Black-box-studio/backendmusic.git`. From its root on a Docker host:

```sh
docker compose -f deploy/compose.yml up -d --build
```

The API is exposed only on the host's loopback port 8787. Put a TLS reverse proxy in front, using the real domain and a valid certificate. Allow request bodies of at least 32 MiB and upload/provider timeouts of 120 seconds. The provider must support persistent storage and the running C++ process; ephemeral/static hosting is unsuitable.

The named `music-data` volume contains the SQLite account/catalog index, encrypted connection configurations, `storage.key`, local uploads and media cache. Back up the database and key together. Never commit `.runtime`, databases, signing keys or passwords. Do not run multiple API replicas against this SQLite volume. Cache retention and storage quotas must be configured operationally before wider use.

The local test administrator is NOT seeded into the container. After the API initializes the data volume, create the hosted administrator interactively:

```sh
docker compose -f deploy/compose.yml exec api python3 /opt/glass/set-admin-password.py admin@gmail.com --db /data/music.sqlite3
```

Enter the desired password at the prompt. Open the app against the hosted HTTPS origin and use **Admin studio → Storage** to configure providers. Hosted connections need their own credentials; no local account/configuration is copied automatically.

## Release gate

Before publishing an APK or desktop installer:

1. Confirm HTTPS health, signup/login and admin authorization on the hosted API.
2. Verify persistent data after container restart and test a real upload to each selected storage provider.
3. Build clients with `GLASS_API_ORIGIN` set to the actual HTTPS origin; no localhost defaults in a public release.
4. Test login, streaming/seeking, upload and TLS on a physical Android phone and each supported desktop OS.
5. Decide how to provide YouTube on mobile: the current Python provider is desktop-only. Android background services/lock-screen controls are also still pending.
6. Use the intended distribution signing identity. The existing local APK is a test candidate, not a hosted production release.

No release automation has been enabled. Hosting access and its HTTPS origin are still required to deploy and verify these steps.
