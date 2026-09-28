#pragma once
#include <string>
namespace Security {
std::string randomHex(int bytes);
std::string digest(const std::string &text);
std::string passwordHash(const std::string &password, const std::string &salt);
bool equal(const std::string &a, const std::string &b);
}
