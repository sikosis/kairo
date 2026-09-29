#pragma once

#include "kairo/model.h"

#include <filesystem>
#include <string>
#include <vector>

namespace kairo {

struct Session {
    int version = 1;
    std::string id;
    std::string project;
    std::string model;
    std::vector<Message> messages;
};

class SessionStore {
public:
    explicit SessionStore(std::filesystem::path directory);
    Session Create(const std::filesystem::path& project, const std::string& model) const;
    void Save(const Session& session) const;
    Session Load(const std::string& id) const;
    std::vector<std::string> List() const;
    const std::filesystem::path& Directory() const { return directory_; }

private:
    std::filesystem::path SessionPath(const std::string& id) const;
    std::filesystem::path directory_;
};

}  // namespace kairo
