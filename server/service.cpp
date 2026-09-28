#include "service.h"
#include "security.h"
#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <sstream>

using namespace drogon;
namespace {
struct ApiError : std::runtime_error {
    int status;
    ApiError(int code, const std::string &message) : std::runtime_error(message), status(code) {}
};
HttpResponsePtr json(const Json::Value &value, int status = 200) {
    auto response = HttpResponse::newHttpJsonResponse(value);
    response->setStatusCode(static_cast<HttpStatusCode>(status));
    response->addHeader("Cache-Control", "no-store");
    return response;
}
Json::Value body(const HttpRequestPtr &request) {
    if (request->body().size() > 16 * 1024) throw ApiError(413, "JSON request is too large.");
    auto parsed = request->getJsonObject();
    if (!parsed || !parsed->isObject()) throw ApiError(400, "Expected a JSON object.");
    return *parsed;
}
std::string field(const Json::Value &value, const char *key, size_t min, size_t max) {
    if (!value[key].isString()) throw ApiError(400, std::string("Missing or invalid ") + key + ".");
    auto text = value[key].asString();
    if (text.size() < min || text.size() > max || text.find('\0') != std::string::npos)
        throw ApiError(400, std::string("Invalid length for ") + key + ".");
    return text;
}
int parseId(const std::string &value) {
    if (value.empty() || value.size() > 9 || !std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); }))
        throw ApiError(400, "Invalid identifier.");
    const int id = std::stoi(value);
    if (id < 1) throw ApiError(400, "Invalid identifier.");
    return id;
}
std::vector<std::string> segments(const std::string &path) {
    std::vector<std::string> out;
    size_t start = 1, end;
    while ((end = path.find('/', start)) != std::string::npos) { out.push_back(path.substr(start, end - start)); start = end + 1; }
    out.push_back(path.substr(start));
    return out;
}
std::string token(const HttpRequestPtr &request) {
    const auto &value = request->getHeader("authorization");
    if (value.size() != 71 || value.compare(0, 7, "Bearer ") != 0) throw ApiError(401, "Sign in to continue.");
    return value.substr(7);
}
}
Service::Service(const std::string &database, const std::string &assets)
    : m_database(database), m_storage(std::filesystem::absolute(std::filesystem::path(database)).parent_path()), m_assets(std::filesystem::canonical(assets))
{
    m_uploads = std::filesystem::absolute(std::filesystem::path(database)).parent_path() / "uploads";
    std::filesystem::create_directories(m_uploads);
    m_uploads = std::filesystem::canonical(m_uploads);
    std::filesystem::permissions(m_uploads, std::filesystem::perms::owner_all);
    if(m_database.query("SELECT id FROM storages WHERE id=1").empty()) {
        Json::Value config;config["directory"]=m_uploads.string();
        m_database.execute("INSERT INTO storages(id,name,provider,config) VALUES(1,'Local library','local',?)",{m_storage.seal(config)});
    }
    for(const auto &directory:{"cache","staging","storage"}) {
        std::filesystem::create_directories(m_storage.root/directory);
        std::filesystem::permissions(m_storage.root/directory,std::filesystem::perms::owner_all);
    }
    std::ifstream homeFile(m_assets / "home.json");
    if (!(homeFile >> m_home) || !m_home.isObject()) throw std::runtime_error("Invalid home catalog");
    for (const auto &name : {"aurora.wav", "afterhours.wav", "coastline.wav"})
        if (!std::filesystem::is_regular_file(m_assets / name)) throw std::runtime_error("Missing bundled audio asset");
}
HttpResponsePtr Service::handle(const HttpRequestPtr &request)
{
    HttpResponsePtr response;
    try { response = route(request); }
    catch (const ApiError &error) { Json::Value value; value["error"] = error.what(); response = json(value, error.status); }
    catch (const std::exception &error) {
        LOG_ERROR << "API failure: " << error.what();
        Json::Value value; value["error"] = "The server could not complete the request."; response = json(value, 500);
    }
    response->addHeader("X-Content-Type-Options", "nosniff");
    return response;
}
HttpResponsePtr Service::authenticate(const HttpRequestPtr &request, bool create)
{
    {
        std::lock_guard<std::mutex> lock(m_rateMutex);
        const auto now = std::time(nullptr);
        for (auto it = m_attempts.begin(); it != m_attempts.end(); ) {
            if (now - it->second.first >= 60) it = m_attempts.erase(it); else ++it;
        }
        if (m_attempts.size() > 10000) throw ApiError(429, "Try again in a minute.");
        auto &entry = m_attempts[request->peerAddr().toIp()];
        if (!entry.first) entry.first = now;
        if (++entry.second > 20) throw ApiError(429, "Too many sign-in attempts. Try again in a minute.");
    }
    const auto data = body(request);
    auto email = field(data, "email", 3, 254);
    std::transform(email.begin(), email.end(), email.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (!std::regex_match(email, std::regex(R"([^\s@]+@[^\s@]+\.[^\s@]+)"))) throw ApiError(400, "Enter a valid email address.");
    const auto password = field(data, "password", 10, 128);
    std::string name, salt, expected;
    int user = 0;
    if (create) {
        name = field(data, "name", 1, 60);
        if (name.find_first_not_of(" \t\r\n") == std::string::npos) throw ApiError(400, "Enter your name.");
        salt = Security::randomHex(16);
    } else {
        std::lock_guard<std::mutex> lock(m_database.mutex);
        auto rows = m_database.query("SELECT * FROM users WHERE email=?", {email});
        if (!rows.empty()) { user = rows[0]["id"].asInt(); name = rows[0]["name"].asString(); salt = rows[0]["salt"].asString(); expected = rows[0]["password_hash"].asString(); }
        else salt = "00000000000000000000000000000000"; // Equal-cost nonexistent user path.
    }
    const auto derived = Security::passwordHash(password, salt); // No database mutex during expensive hashing.
    if (!create && (!user || !Security::equal(derived, expected))) throw ApiError(401, "Email or password is incorrect.");
    const auto bearer = Security::randomHex(32);
    std::lock_guard<std::mutex> lock(m_database.mutex);
    if (create) {
        if (!m_database.query("SELECT id FROM users WHERE email=?", {email}).empty()) throw ApiError(409, "An account already uses that email.");
        m_database.execute("INSERT INTO users(email,name,salt,password_hash) VALUES(?,?,?,?)", {email,name,salt,derived});
        user = m_database.lastId();
    }
    m_database.execute("DELETE FROM sessions WHERE expires<=?", {std::to_string(std::time(nullptr))});
    m_database.execute("INSERT INTO sessions VALUES(?,?,?)", {Security::digest(bearer), std::to_string(user), std::to_string(std::time(nullptr) + 86400)});
    Json::Value result;
    result["token"] = bearer;
    result["user"] = profile(user);
    return json(result, create ? 201 : 200);
}
int Service::userId(const HttpRequestPtr &request)
{
    auto rows = m_database.query("SELECT user_id FROM sessions WHERE token_hash=? AND expires>?", {Security::digest(token(request)),std::to_string(std::time(nullptr))});
    if (rows.empty()) throw ApiError(401, "Your session expired. Please sign in again.");
    return rows[0]["user_id"].asInt();
}
Json::Value Service::library(int user)
{
    const auto key = std::to_string(user);
    Json::Value result;
    result["likes"] = Json::Value(Json::arrayValue);
    for (const auto &row : m_database.query("SELECT track_id FROM likes WHERE user_id=? ORDER BY track_id", {key})) result["likes"].append(row["track_id"]);
    auto lists = m_database.query("SELECT id,name,pinned FROM playlists WHERE user_id=? ORDER BY pinned DESC,id DESC", {key});
    for (auto &list : lists) {
        list["trackIds"] = Json::Value(Json::arrayValue);
        for (const auto &row : m_database.query("SELECT track_id FROM playlist_tracks WHERE playlist_id=? ORDER BY position", {list["id"].asString()})) list["trackIds"].append(row["track_id"]);
    }
    result["playlists"] = lists;
    return result;
}
HttpResponsePtr Service::stream(const HttpRequestPtr &request, int id)
{
    std::string filename, objectKey; int storageId=1;
    {
        std::lock_guard<std::mutex> lock(m_database.mutex);
        validateTicket(request,id);
        const auto rows = m_database.query("SELECT filename,storage_id,object_key FROM tracks WHERE id=?", {std::to_string(id)});
        if (rows.empty()) throw ApiError(404, "Track not found.");
        filename = rows[0]["filename"].asString();
        storageId=rows[0]["storage_id"].asInt(); objectKey=rows[0]["object_key"].asString();
    }
    // Only server-owned filenames are usable; no request path is joined to disk.
    const bool uploaded = filename.rfind("uploads/", 0) == 0;
    const auto root = objectKey.empty() ? (uploaded ? m_uploads : m_assets) : m_storage.root/"cache";
    auto target = root / (uploaded ? filename.substr(8) : filename);
    if(!objectKey.empty() && !std::filesystem::exists(target)) {
        const auto temporary = target.string()+"."+Security::randomHex(6)+".part";
        Json::Value command; command["action"]="download";command["key"]=objectKey;command["path"]=temporary;
        try { storageCall(storageId,command); std::filesystem::rename(temporary,target); }
        catch(...) { std::error_code ignored;std::filesystem::remove(temporary,ignored);throw; }
    }
    const auto path = std::filesystem::canonical(target);
    if (path.parent_path() != root) throw ApiError(404, "Track not found.");
    const auto ext = path.extension().string();
    const std::string mime = ext == ".mp3" ? "audio/mpeg" : ext == ".flac" ? "audio/flac" : ext == ".ogg" ? "audio/ogg" : ext == ".m4a" ? "audio/mp4" : "audio/wav";
    const auto size = std::filesystem::file_size(path);
    const auto &range = request->getHeader("range");
    HttpResponsePtr response;
    if (range.empty()) response = HttpResponse::newFileResponse(path.string(), "", CT_CUSTOM, mime, request);
    else {
        std::smatch match;
        if (!std::regex_match(range, match, std::regex(R"(bytes=([0-9]*)-([0-9]*))")) || (match[1].str().empty() && match[2].str().empty())) {
            response = HttpResponse::newHttpResponse(); response->setStatusCode(k416RequestedRangeNotSatisfiable);
        } else {
            try {
                const auto left = match[1].str(), right = match[2].str();
                auto start = left.empty() ? (size - std::min<uint64_t>(size, std::stoull(right))) : std::stoull(left);
                auto end = left.empty() || right.empty() ? size - 1 : std::min<uint64_t>(size - 1, std::stoull(right));
                if (start >= size || start > end) throw std::out_of_range("range");
                response = HttpResponse::newFileResponse(path.string(), start, end - start + 1, true, "", CT_CUSTOM, mime, request);
                response->setStatusCode(k206PartialContent);
            } catch (const std::exception &) { response = HttpResponse::newHttpResponse(); response->setStatusCode(k416RequestedRangeNotSatisfiable); }
        }
        if (response->statusCode() == k416RequestedRangeNotSatisfiable) response->addHeader("Content-Range", "bytes */" + std::to_string(size));
    }
    response->addHeader("Accept-Ranges", "bytes");
    response->addHeader("Cache-Control", "private, no-store");
    return response;
}
HttpResponsePtr Service::route(const HttpRequestPtr &request)
{
    const auto path = request->path();
    const auto method = request->method();

    if (path == "/api/v1/health" && method == Get) { Json::Value out; out["status"] = "ok"; return json(out); }
    if (path == "/api/v1/auth/register" && method == Post) return authenticate(request, true);
    if (path == "/api/v1/auth/login" && method == Post) return authenticate(request, false);
    const auto parts = segments(path);
    if (parts.size() == 5 && parts[2] == "tracks" && parts[4] == "stream" && (method == Get || method == Head)) return stream(request, parseId(parts[3]));
    std::unique_lock<std::mutex> lock(m_database.mutex);
    if(path=="/api/v1/onboarding" && method==Get) {
        Json::Value out; std::istringstream input(m_database.query("SELECT value FROM app_settings WHERE key='onboarding'")[0]["value"].asString()); input>>out["slides"]; return json(out);
    }
    const auto user = userId(request);
    const auto key = std::to_string(user);
    if (path == "/api/v1/home" && method == Get) {
        Json::Value home;
        home["backgroundColor"] = m_database.query("SELECT value FROM app_settings WHERE key='backgroundColor'")[0]["value"];
        return json(home);
    }
    if (path == "/api/v1/tracks" && method == Get) {
        const auto search = request->getParameter("q");
        if (search.size() > 200) throw ApiError(400, "Search is too long.");
        const auto genre = request->getParameter("genre");
        auto rows = m_database.query("SELECT tracks.id,title,artist,album,genre,color1,color2,duration,storage_id,COALESCE(storages.name,'Local library') AS storageName FROM tracks LEFT JOIN storages ON tracks.storage_id=storages.id WHERE (instr(lower(title || ' ' || artist || ' ' || album),lower(?))>0) AND (?='' OR genre=?) ORDER BY tracks.id DESC LIMIT 500", {search,genre,genre});
        for (auto &row : rows) row["streamPath"] = "/api/v1/tracks/" + row["id"].asString() + "/stream?t="+ticket(row["id"].asInt(),user,Security::digest(token(request)));
        Json::Value out; out["tracks"] = rows; return json(out);
    }
    if (path == "/api/v1/auth/logout" && method == Post) {
        m_database.execute("DELETE FROM sessions WHERE token_hash=?", {Security::digest(token(request))});
        Json::Value out; out["ok"] = true; return json(out);
    }
    if (path == "/api/v1/me" && method == Get) { Json::Value out; out["user"] = profile(user); return json(out); }
    if (path == "/api/v1/me" && method == Patch) {
        const auto data = body(request);
        const auto name = field(data, "name", 1, 60), bio = field(data, "bio", 0, 280);
        if (name.find_first_not_of(" \t\r\n") == std::string::npos) throw ApiError(400, "Enter a display name.");
        if (!data["backgroundPlay"].isBool() || !data["reducedMotion"].isBool()) throw ApiError(400, "Invalid playback preferences.");
        m_database.execute("BEGIN IMMEDIATE");
        try {
            m_database.execute("UPDATE users SET name=? WHERE id=?", {name,key});
            m_database.execute("INSERT INTO preferences VALUES(?,?,?,?) ON CONFLICT(user_id) DO UPDATE SET bio=excluded.bio,background_play=excluded.background_play,reduced_motion=excluded.reduced_motion", {key,bio,data["backgroundPlay"].asBool() ? "1" : "0",data["reducedMotion"].asBool() ? "1" : "0"});
            m_database.execute("COMMIT");
        } catch (...) { m_database.execute("ROLLBACK"); throw; }
        Json::Value out; out["user"] = profile(user); return json(out);
    }
    if (path.rfind("/api/v1/admin/", 0) == 0) {
        if (profile(user)["role"].asString() != "admin") throw ApiError(403, "Administrator access required.");
        if (path == "/api/v1/admin/tracks" && method == Post) { lock.unlock(); return upload(request); }
        if(parts.size()>=4 && parts[3]=="storages") { lock.unlock(); return storageRoute(request,parts); }
        if(path=="/api/v1/admin/onboarding" && method==Put) {
            auto slides=body(request)["slides"];
            if(!slides.isArray() || slides.empty() || slides.size()>6) throw ApiError(400,"Use between one and six welcome slides.");
            for(const auto &slide:slides) {
                field(slide,"title",1,80);field(slide,"body",1,240);
                if(!std::regex_match(field(slide,"color",7,7),std::regex("#[0-9a-fA-F]{6}"))) throw ApiError(400,"Invalid slide color.");
            }
            m_database.execute("UPDATE app_settings SET value=? WHERE key='onboarding'",{Json::writeString(Json::StreamWriterBuilder(),slides)});
            Json::Value out;out["slides"]=slides;return json(out);
        }
        if (path == "/api/v1/admin/theme" && method == Put) {
            const auto color = field(body(request), "backgroundColor", 7, 7);
            if (!std::regex_match(color, std::regex("#[0-9a-fA-F]{6}"))) throw ApiError(400, "Use a six-digit hex color.");
            m_database.execute("UPDATE app_settings SET value=? WHERE key='backgroundColor'", {color});
            Json::Value out; out["backgroundColor"] = color; return json(out);
        }
        throw ApiError(404, "Endpoint not found.");
    }
    if(path=="/api/v1/catalog/refresh" && method==Post) {
        lock.unlock(); return storageRoute(request, {"api","v1","admin","storages","sync"});
    }
    if (path == "/api/v1/library" && method == Get) return json(library(user));
    if (parts.size() == 4 && parts[2] == "likes" && (method == Put || method == Delete)) {
        const auto id = std::to_string(parseId(parts[3]));
        if (m_database.query("SELECT id FROM tracks WHERE id=?", {id}).empty()) throw ApiError(404, "Track not found.");
        if (method == Put) m_database.execute("INSERT OR IGNORE INTO likes VALUES(?,?)", {key,id});
        else m_database.execute("DELETE FROM likes WHERE user_id=? AND track_id=?", {key,id});
        return json(library(user));
    }
    if (path == "/api/v1/playlists" && method == Post) {
        const auto name = field(body(request), "name", 1, 80);
        if (name.find_first_not_of(" \t\r\n") == std::string::npos) throw ApiError(400, "Give your playlist a name.");
        if (m_database.query("SELECT id FROM playlists WHERE user_id=?", {key}).size() >= 100) throw ApiError(409, "Playlist limit reached.");
        m_database.execute("INSERT INTO playlists(user_id,name) VALUES(?,?)", {key,name});
        return json(library(user), 201);
    }
    if (parts.size() >= 4 && parts[2] == "playlists") {
        const auto playlist = std::to_string(parseId(parts[3]));
        if (m_database.query("SELECT id FROM playlists WHERE id=? AND user_id=?", {playlist,key}).empty()) throw ApiError(404, "Playlist not found.");
        if (parts.size() == 4 && method == Delete) m_database.execute("DELETE FROM playlists WHERE id=? AND user_id=?", {playlist,key});
        else if (parts.size() == 4 && method == Patch) {
            const auto payload=body(request);
            if(payload.isMember("pinned")) {
                if(!payload["pinned"].isBool()) throw ApiError(400,"Invalid pin state.");
                m_database.execute("UPDATE playlists SET pinned=? WHERE id=? AND user_id=?",{payload["pinned"].asBool()?"1":"0",playlist,key});
                return json(library(user));
            }
            const auto name = field(payload, "name", 1, 80);
            if (name.find_first_not_of(" \t\r\n") == std::string::npos) throw ApiError(400, "Give your playlist a name.");
            m_database.execute("UPDATE playlists SET name=? WHERE id=? AND user_id=?", {name,playlist,key});
        } else if (parts.size() == 6 && parts[4] == "tracks" && (method == Put || method == Delete)) {
            const auto id = std::to_string(parseId(parts[5]));
            if (m_database.query("SELECT id FROM tracks WHERE id=?", {id}).empty()) throw ApiError(404, "Track not found.");
            if (method == Put) m_database.execute("INSERT OR IGNORE INTO playlist_tracks SELECT ?,?,COALESCE(MAX(position),-1)+1 FROM playlist_tracks WHERE playlist_id=?", {playlist,id,playlist});
            else m_database.execute("DELETE FROM playlist_tracks WHERE playlist_id=? AND track_id=?", {playlist,id});
        } else throw ApiError(404, "Endpoint not found.");
        return json(library(user));
    }
    throw ApiError(404, "Endpoint not found.");
}

Json::Value Service::profile(int user)
{
    const auto key = std::to_string(user);
    auto result = m_database.query("SELECT id,name,email,role FROM users WHERE id=?", {key})[0];
    auto prefs = m_database.query("SELECT bio,background_play,reduced_motion FROM preferences WHERE user_id=?", {key});
    result["bio"] = prefs.empty() ? "" : prefs[0]["bio"].asString();
    result["backgroundPlay"] = prefs.empty() || prefs[0]["background_play"].asInt() != 0;
    result["reducedMotion"] = !prefs.empty() && prefs[0]["reduced_motion"].asInt() != 0;
    return result;
}
HttpResponsePtr Service::upload(const HttpRequestPtr &request)
{
    // Called only after authentication/role authorization, under the database mutex.
    Json::Value metadata;
    for (const auto &key : {"title", "artist", "album", "genre"}) metadata[key] = request->getParameter(key);
    const auto title = field(metadata,"title",1,120), artist = field(metadata,"artist",1,120);
    const auto album = field(metadata,"album",1,120), genre = field(metadata,"genre",1,40);
    for (const auto &value : {title,artist,album,genre})
        if (value.find_first_not_of(" \t\r\n") == std::string::npos) throw ApiError(400,"Track details cannot be blank.");
    const auto bytes = request->body();
    if (bytes.size() < 44 || bytes.size() > 32 * 1024 * 1024) throw ApiError(413,"Choose an audio file between 44 bytes and 32 MiB.");
    std::string extension;
    if (bytes.substr(0,4) == "RIFF" && bytes.substr(8,4) == "WAVE") extension = ".wav";
    else if (bytes.substr(0,4) == "fLaC") extension = ".flac";
    else if (bytes.substr(0,4) == "OggS") extension = ".ogg";
    else if (bytes.substr(0,3) == "ID3" || (static_cast<unsigned char>(bytes[0]) == 0xff && (static_cast<unsigned char>(bytes[1]) & 0xe0) == 0xe0)) extension = ".mp3";
    else if (bytes.substr(4,4) == "ftyp" && (bytes.substr(8,4) == "M4A " || bytes.substr(8,4) == "isom" || bytes.substr(8,4) == "mp42")) extension = ".m4a";
    else throw ApiError(415,"Unsupported audio. Choose WAV, MP3, FLAC, Ogg, or M4A.");
    const int storageId=parseId(request->getParameter("storageId"));
    const auto name=Security::randomHex(16)+extension;
    const auto path=m_storage.root/"staging"/name;
    metadata["title"]=title;metadata["artist"]=artist;metadata["album"]=album;metadata["genre"]=genre;metadata["extension"]=extension;
    metadata["mime"]=extension==".mp3"?"audio/mpeg":extension==".flac"?"audio/flac":extension==".ogg"?"audio/ogg":extension==".m4a"?"audio/mp4":"audio/wav";
    std::string objectKey;
    try {
        std::ofstream file(path,std::ios::binary);file.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));file.close();
        if(!file) throw std::runtime_error("Cannot stage audio");
        Json::Value probe;probe["action"]="probe";probe["path"]=path.string();
        auto info=m_storage.run(probe);if(info.isMember("error")) throw ApiError(415,info["error"].asString());
        metadata["duration"]=info["duration"];
        Json::Value command;command["action"]="upload";command["path"]=path.string();command["key"]=name;command["metadata"]=metadata;
        objectKey=storageCall(storageId,command)["key"].asString();
        std::lock_guard<std::mutex> guard(m_database.mutex);
        m_database.execute("INSERT INTO tracks(title,artist,album,genre,filename,color1,color2,duration,storage_id,object_key) VALUES(?,?,?,?,?,'#659b86','#233b4c',?,?,?)", {title,artist,album,genre,name,std::to_string(metadata["duration"].asInt()),std::to_string(storageId),objectKey});
        Json::Value out;out["id"]=m_database.lastId();out["title"]=title;out["duration"]=metadata["duration"];
        std::filesystem::remove(path);return json(out,201);
    } catch(...) {
        std::error_code ignored;std::filesystem::remove(path,ignored);
        if(!objectKey.empty()) { try { Json::Value command;command["action"]="delete";command["key"]=objectKey;storageCall(storageId,command); } catch(...) {} }
        throw;
    }
}

std::string Service::ticket(int track,int user,const std::string &session) {
    auto payload=std::to_string(std::time(nullptr)+86400)+"."+std::to_string(user)+"."+session;
    return payload+"."+m_storage.sign(std::to_string(track)+"."+payload);
}
void Service::validateTicket(const HttpRequestPtr &request,int track) {
    if(!request->getHeader("authorization").empty()) { userId(request);return; }
    auto value=request->getParameter("t");std::smatch match;
    if(!std::regex_match(value,match,std::regex(R"(([0-9]{10})\.([0-9]{1,9})\.([a-f0-9]{64})\.([a-f0-9]{64}))"))) throw ApiError(401,"Sign in to stream music.");
    const auto payload=match[1].str()+"."+match[2].str()+"."+match[3].str();
    if(std::stoll(match[1].str())<=std::time(nullptr) || !Security::equal(match[4].str(),m_storage.sign(std::to_string(track)+"."+payload)) || m_database.query("SELECT user_id FROM sessions WHERE token_hash=? AND user_id=? AND expires>?",{match[3],match[2],std::to_string(std::time(nullptr))}).empty()) throw ApiError(401,"The playback session expired.");
}
Json::Value Service::storageList() {
    Json::Value out;out["storages"]=m_database.query("SELECT id,name,provider,(SELECT count(*) FROM tracks WHERE storage_id=storages.id) AS trackCount FROM storages ORDER BY id");return out;
}
Json::Value Service::storageCall(int id,Json::Value request) {
    {
        std::lock_guard<std::mutex> guard(m_database.mutex);
        auto rows=m_database.query("SELECT provider,config FROM storages WHERE id=?",{std::to_string(id)});
        if(rows.empty()) throw ApiError(404,"Storage connection not found.");
        request["provider"]=rows[0]["provider"];request["config"]=m_storage.open(rows[0]["config"].asString());
    }
    auto result=m_storage.run(request);if(result.isMember("error")) throw ApiError(502,result["error"].asString());return result;
}
HttpResponsePtr Service::storageRoute(const HttpRequestPtr &request,const std::vector<std::string> &parts) {
    if(parts.size()==4 && request->method()==Get) { std::lock_guard<std::mutex> guard(m_database.mutex);return json(storageList()); }
    if(parts.size()==4 && request->method()==Post) {
        auto data=body(request);auto name=field(data,"name",1,80),provider=field(data,"provider",1,20);auto config=data["config"];
        if(!config.isObject()) throw ApiError(400,"Storage configuration is required.");
        if(provider=="local") config["directory"]=(m_storage.root/"storage"/Security::randomHex(12)).string();
        else if(provider=="mongodb") { field(config,"host",3,254);field(config,"username",1,128);field(config,"password",1,256);field(config,"database",1,64); }
        else if(provider=="firebase") { field(config,"projectId",1,128);field(config,"bucket",1,254);field(config,"credentials",10,12000); }
        else if(provider=="gdrive") { field(config,"folderId",1,128);field(config,"credentials",10,12000); }
        else if(provider=="s3") { field(config,"bucket",1,254);field(config,"accessKey",1,256);field(config,"secretKey",1,512); }
        else throw ApiError(400,"Unsupported storage provider.");
        Json::Value command;command["action"]="test";command["provider"]=provider;command["config"]=config;
        auto result=m_storage.run(command);if(result.isMember("error")) throw ApiError(400,result["error"].asString());
        std::lock_guard<std::mutex> guard(m_database.mutex);
        m_database.execute("INSERT INTO storages(name,provider,config) VALUES(?,?,?)",{name,provider,m_storage.seal(config)});return json(storageList(),201);
    }
    if(parts.size()==5 && parts[4]=="sync" && request->method()==Post) {
        Json::Value rows;{std::lock_guard<std::mutex> guard(m_database.mutex);rows=m_database.query("SELECT id,name FROM storages");}
        Json::Value out;out["errors"]=Json::Value(Json::arrayValue);int added=0;
        for(const auto &row:rows) {
            try {
                Json::Value command;command["action"]="scan";auto data=storageCall(row["id"].asInt(),command);
                for(const auto &track:data["tracks"]) {
                    auto key=field(track,"key",1,256),ext=field(track,"extension",4,5);
                    if(!std::regex_match(key,std::regex("[A-Za-z0-9_.-]+")) || !std::regex_match(ext,std::regex(R"(\.(wav|mp3|flac|ogg|m4a))"))) continue;
                    if(!track["duration"].isNumeric() || track["duration"].asDouble()<=0) continue;
                    std::lock_guard<std::mutex> guard(m_database.mutex);
                    if(!m_database.query("SELECT id FROM tracks WHERE storage_id=? AND object_key=?",{row["id"].asString(),key}).empty())continue;
                    m_database.execute("INSERT INTO tracks(title,artist,album,genre,filename,color1,color2,duration,storage_id,object_key) VALUES(?,?,?,?,?,'#7663b6','#183d59',?,?,?)",{field(track,"title",1,120),field(track,"artist",1,120),field(track,"album",1,120),field(track,"genre",1,40),Security::digest(row["id"].asString()+key)+ext,std::to_string(track["duration"].asInt()),row["id"].asString(),key});++added;
                }
            } catch(const std::exception &) { out["errors"].append(row["name"].asString()+": sync unavailable; check connection in Admin studio."); }
        }
        out["added"]=added;return json(out);
    }
    if(parts.size()==5) {
        auto id=std::to_string(parseId(parts[4]));
        if(request->method()==Post) { Json::Value command;command["action"]="test";storageCall(std::stoi(id),command);Json::Value out;out["ok"]=true;return json(out); }
        std::lock_guard<std::mutex> guard(m_database.mutex);
        if(m_database.query("SELECT id FROM storages WHERE id=?",{id}).empty())throw ApiError(404,"Storage not found.");
        if(request->method()==Patch) m_database.execute("UPDATE storages SET name=? WHERE id=?",{field(body(request),"name",1,80),id});
        else if(request->method()==Delete) {
            if(id=="1" || !m_database.query("SELECT id FROM tracks WHERE storage_id=?",{id}).empty())throw ApiError(409,"A storage connection with songs cannot be removed. The default local connection is permanent.");
            m_database.execute("DELETE FROM storages WHERE id=?",{id});
        } else throw ApiError(404,"Endpoint not found.");
        return json(storageList());
    }
    throw ApiError(404,"Endpoint not found.");
}
