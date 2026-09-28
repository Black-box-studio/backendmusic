#include "database.h"
#include <memory>
#include <stdexcept>

Database::Database(const std::string &path)
{
    if (sqlite3_open_v2(path.c_str(), &m_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        const std::string message = m_db ? sqlite3_errmsg(m_db) : "Cannot open database";
        sqlite3_close(m_db);
        throw std::runtime_error(message);
    }
    sqlite3_busy_timeout(m_db, 5000);
    execute("PRAGMA foreign_keys=ON");
    execute("PRAGMA journal_mode=WAL");
    execute("CREATE TABLE IF NOT EXISTS users(id INTEGER PRIMARY KEY, email TEXT NOT NULL UNIQUE, name TEXT NOT NULL, salt TEXT NOT NULL, password_hash TEXT NOT NULL)");
    execute("CREATE TABLE IF NOT EXISTS sessions(token_hash TEXT PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE, expires INTEGER NOT NULL)");
    execute("CREATE TABLE IF NOT EXISTS tracks(id INTEGER PRIMARY KEY, title TEXT NOT NULL, artist TEXT NOT NULL, album TEXT NOT NULL, genre TEXT NOT NULL, filename TEXT NOT NULL, color1 TEXT NOT NULL, color2 TEXT NOT NULL, duration INTEGER NOT NULL)");
    execute("CREATE TABLE IF NOT EXISTS likes(user_id INTEGER REFERENCES users(id) ON DELETE CASCADE, track_id INTEGER REFERENCES tracks(id) ON DELETE CASCADE, PRIMARY KEY(user_id,track_id))");
    execute("CREATE TABLE IF NOT EXISTS playlists(id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE, name TEXT NOT NULL)");
    execute("CREATE TABLE IF NOT EXISTS playlist_tracks(playlist_id INTEGER REFERENCES playlists(id) ON DELETE CASCADE, track_id INTEGER REFERENCES tracks(id) ON DELETE CASCADE, position INTEGER NOT NULL, PRIMARY KEY(playlist_id,track_id))");
    // Idempotent migration for databases created by version 2.
    bool hasRole = false;
    for (const auto &column : query("PRAGMA table_info(users)"))
        if (column["name"].asString() == "role") hasRole = true;
    if (!hasRole) execute("ALTER TABLE users ADD COLUMN role TEXT NOT NULL DEFAULT 'listener'");
    execute("CREATE TABLE IF NOT EXISTS preferences(user_id INTEGER PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE, bio TEXT NOT NULL DEFAULT '', background_play INTEGER NOT NULL DEFAULT 1, reduced_motion INTEGER NOT NULL DEFAULT 0)");
    execute("CREATE TABLE IF NOT EXISTS app_settings(key TEXT PRIMARY KEY, value TEXT NOT NULL)");
    execute("INSERT OR IGNORE INTO app_settings VALUES('backgroundColor','#3333a3')");
    execute("CREATE INDEX IF NOT EXISTS playlists_owner ON playlists(user_id)");
    execute("CREATE INDEX IF NOT EXISTS session_expiry ON sessions(expires)");
    auto column = [this](const std::string &table, const std::string &name, const std::string &definition) {
        for(const auto &item : query("PRAGMA table_info("+table+")")) if(item["name"].asString()==name) return;
        execute("ALTER TABLE "+table+" ADD COLUMN "+name+" "+definition);
    };
    column("tracks","storage_id","INTEGER NOT NULL DEFAULT 1");
    column("tracks","object_key","TEXT NOT NULL DEFAULT ''");
    column("playlists","pinned","INTEGER NOT NULL DEFAULT 0");
    execute("CREATE UNIQUE INDEX IF NOT EXISTS storage_object ON tracks(storage_id,object_key) WHERE object_key<>''");
    execute("CREATE TABLE IF NOT EXISTS storages(id INTEGER PRIMARY KEY, name TEXT NOT NULL, provider TEXT NOT NULL, config TEXT NOT NULL)");
    execute("INSERT OR IGNORE INTO app_settings VALUES('onboarding',?)", {R"([{"title":"Your music. Your atmosphere.","body":"One beautifully personal home for the songs you love.","color":"#7048d8"},{"title":"A world beyond your library.","body":"Explore YouTube Music by mood, genre and the artists that move you.","color":"#246a98"},{"title":"Bring every collection together.","body":"Connected storage, personal playlists and a glow that follows your music.","color":"#467978"}])"});
    // Fresh installs start with an empty catalog. Existing user data is preserved.

}
Database::~Database() { sqlite3_close(m_db); }
Json::Value Database::query(const std::string &sql, const std::vector<std::string> &args)
{
    sqlite3_stmt *raw = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(m_db));
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(raw, sqlite3_finalize);
    for (size_t i = 0; i < args.size(); ++i)
        if (sqlite3_bind_text(raw, int(i + 1), args[i].c_str(), int(args[i].size()), SQLITE_TRANSIENT) != SQLITE_OK)
            throw std::runtime_error("Cannot bind database value");
    Json::Value rows(Json::arrayValue);
    int status;
    while ((status = sqlite3_step(raw)) == SQLITE_ROW) {
        Json::Value row(Json::objectValue);
        for (int i = 0; i < sqlite3_column_count(raw); ++i) {
            const char *name = sqlite3_column_name(raw, i);
            if (sqlite3_column_type(raw, i) == SQLITE_INTEGER)
                row[name] = Json::Int64(sqlite3_column_int64(raw, i));
            else if (sqlite3_column_type(raw, i) == SQLITE_NULL)
                row[name] = Json::nullValue;
            else
                row[name] = reinterpret_cast<const char *>(sqlite3_column_text(raw, i));
        }
        rows.append(row);
    }
    if (status != SQLITE_DONE)
        throw std::runtime_error(sqlite3_errmsg(m_db));
    return rows;
}
void Database::execute(const std::string &sql, const std::vector<std::string> &args) { query(sql, args); }
int Database::lastId() const { return int(sqlite3_last_insert_rowid(m_db)); }
