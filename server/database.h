#pragma once
#include <sqlite3.h>
#include <json/json.h>
#include <mutex>
#include <string>
#include <vector>

// One FULLMUTEX connection. Service serializes multi-statement operations with
// mutex; all input values go through prepared statements, never SQL concatenation.
class Database final {
public:
    explicit Database(const std::string &path);
    ~Database();
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;
    Json::Value query(const std::string &sql, const std::vector<std::string> &args = {});
    void execute(const std::string &sql, const std::vector<std::string> &args = {});
    int lastId() const;
    std::mutex mutex;
private:
    sqlite3 *m_db = nullptr;
};
