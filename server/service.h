#pragma once
#include "database.h"
#include "storage.h"
#include <drogon/drogon.h>
#include <filesystem>
#include <map>

class Service final {
public:
    Service(const std::string &database, const std::string &assets);
    drogon::HttpResponsePtr handle(const drogon::HttpRequestPtr &request);
private:
    drogon::HttpResponsePtr route(const drogon::HttpRequestPtr &request);
    drogon::HttpResponsePtr authenticate(const drogon::HttpRequestPtr &request, bool create);
    drogon::HttpResponsePtr stream(const drogon::HttpRequestPtr &request, int id);
    int userId(const drogon::HttpRequestPtr &request);
    Json::Value library(int user);
    Json::Value profile(int user);
    drogon::HttpResponsePtr upload(const drogon::HttpRequestPtr &request);
    Json::Value storageList();
    Json::Value storageCall(int id, Json::Value request);
    drogon::HttpResponsePtr storageRoute(const drogon::HttpRequestPtr &request, const std::vector<std::string> &parts);
    std::string ticket(int track, int user, const std::string &session);
    void validateTicket(const drogon::HttpRequestPtr &request, int track);
    Database m_database;
    Storage m_storage;
    Json::Value m_home;
    std::filesystem::path m_assets, m_uploads;
    std::mutex m_rateMutex;
    std::map<std::string, std::pair<std::time_t, int>> m_attempts;
};
