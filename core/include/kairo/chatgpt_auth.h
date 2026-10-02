#pragma once

#include "kairo/cancellation.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace kairo {

struct ChatGPTAccount {
    std::string email;
    std::string subject;
    std::string client_id;
    std::string host_id;
    std::string id_token;
    std::string access_token;
    std::string refresh_token;
    std::string scope;
    long long expires_at = 0;
};

struct ChatGPTModel {
    std::string slug;
    std::string display_name;
};

class ChatGPTAuth {
public:
    using BrowserLauncher = std::function<void(const std::string&)>;

    explicit ChatGPTAuth(std::filesystem::path credential_file);
    bool IsSignedIn() const;
    ChatGPTAccount Account() const;
    ChatGPTAccount SignIn(const BrowserLauncher& launch_browser,
                          const CancellationToken& cancellation);
    void SignOut();
    std::string AccessToken();
    std::vector<ChatGPTModel> ListModels();

private:
    ChatGPTAccount Load() const;
    void Save(const ChatGPTAccount& account) const;
    std::filesystem::path credential_file_;
};

}  // namespace kairo
