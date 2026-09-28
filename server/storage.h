#pragma once
#include <json/json.h>
#include <filesystem>
#include <string>
class Storage final {
public:
    explicit Storage(const std::filesystem::path &root);
    std::string seal(const Json::Value &value) const;
    Json::Value open(const std::string &value) const;
    std::string sign(const std::string &value) const;
    Json::Value run(Json::Value request) const;
    const std::filesystem::path root;
private:
    std::string key;
};
