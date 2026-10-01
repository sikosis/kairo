#pragma once

#include "kairo/provider.h"

#include <memory>
#include <string>
#include <vector>

namespace kairo {

enum class ProviderKind {
    OpenAI,
    AnthropicCompatibility,
    OpenAICompatible,
};

struct ProviderProfile {
    std::string id;
    std::string name;
    ProviderKind kind = ProviderKind::OpenAICompatible;
    std::string base_url;
    std::string api_key_environment;
    std::string api_key;
    std::vector<std::string> models;
    std::string selected_model;
};

const char* ProviderKindName(ProviderKind kind);
std::string ProviderKindId(ProviderKind kind);
ProviderKind ParseProviderKind(const std::string& value);
std::vector<ProviderProfile> DefaultProviderProfiles();
std::shared_ptr<Provider> CreateProvider(const ProviderProfile& profile,
                                         long timeout_seconds = 120);

}  // namespace kairo
