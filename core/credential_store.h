#pragma once
#include <string>

class CredentialStore {
 public:
    static bool save(const std::string &name, const std::string &secret, std::string &error);
    static bool load(const std::string &name, std::string &secret, std::string &error);
    static bool remove(const std::string &name, std::string &error);
};
