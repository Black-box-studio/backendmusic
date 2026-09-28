# Run the test backend on GitHub Codespaces

Codespaces can provide a temporary HTTPS endpoint for phone/desktop debugging. This is a development environment, not an always-on production host. The API is unavailable while the codespace is stopped, and deleting the codespace deletes its local database and encryption key. Included compute/storage quotas apply; keep paid usage disabled for free-only testing and stop the codespace when done. Do not add keep-alive jobs or disable inactivity shutdown.

1. Open this repository on GitHub: **Code → Codespaces → Create codespace on main**. Choose a 2-core machine. Check your included quota and billing settings before creation.
2. Wait for container setup. It builds the server and starts port 8787 automatically.
3. In the Codespaces terminal, create the administrator with a password prompt:
   ```sh
   python3 scripts/set-admin-password.py admin@gmail.com
   ```
4. In **Ports**, change port 8787 visibility to **Public** so the native apps can connect. API authentication still protects accounts, tracks and admin operations. Copy the forwarded **HTTPS** address.
5. In the app's login screen, select **Server connection**, paste the HTTPS origin and save. Sign in and configure storage under **Admin studio → Storage**.
6. Test `/api/v1/health`, login, upload and playback before distributing test builds. The existing mobile YouTube and background-service limitations remain.

The repository contains no admin password or live database. Runtime state lives in the ignored `.runtime/` directory within the codespace workspace. Preserve `music.sqlite3` and `storage.key` together when backing up. Codespaces retention policies may automatically delete stopped environments.

If using GitHub CLI, access requires `gh auth refresh -h github.com -s codespace`. Creation/port changes must use the intended personal account and respect the free-only budget. Configuration alone does not create a codespace or start billing.

References: [Codespaces included usage](https://docs.github.com/en/codespaces/troubleshooting/troubleshooting-included-usage), [port forwarding and lifecycle](https://docs.github.com/en/codespaces/about-codespaces/deep-dive).
