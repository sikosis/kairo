#include "kairo/provider_profile.h"

#include "kairo/chatgpt_provider.h"
#include "kairo/openai_provider.h"

#include <stdexcept>

namespace kairo {

const char* ProviderKindName(ProviderKind kind) {
    switch (kind) {
        case ProviderKind::OpenAI: return "OpenAI API";
        case ProviderKind::AnthropicCompatibility: return "Anthropic API";
        case ProviderKind::OpenAICompatible: return "OpenAI-compatible";
        case ProviderKind::ChatGPTPlan: return "ChatGPT Plus / Pro";
    }
    return "OpenAI-compatible";
}

std::string ProviderKindId(ProviderKind kind) {
    switch (kind) {
        case ProviderKind::OpenAI: return "openai";
        case ProviderKind::AnthropicCompatibility: return "anthropic";
        case ProviderKind::OpenAICompatible: return "openai-compatible";
        case ProviderKind::ChatGPTPlan: return "chatgpt-plan";
    }
    return "openai-compatible";
}

ProviderKind ParseProviderKind(const std::string& value) {
    if (value == "openai") return ProviderKind::OpenAI;
    if (value == "anthropic") return ProviderKind::AnthropicCompatibility;
    if (value == "openai-compatible") return ProviderKind::OpenAICompatible;
    if (value == "chatgpt-plan") return ProviderKind::ChatGPTPlan;
    throw std::runtime_error("unknown provider kind: " + value);
}

bool ProviderUsesApiKey(ProviderKind kind) {
    return kind != ProviderKind::ChatGPTPlan;
}

bool ProviderUsesChatGPTPlan(ProviderKind kind) { return kind == ProviderKind::ChatGPTPlan; }

void ValidateProviderProfile(const ProviderProfile& profile) {
    if (profile.name.empty()) throw std::invalid_argument("provider name is required");
    if (profile.models.empty()) throw std::invalid_argument("provider has no configured models");
    if (profile.base_url.empty()) throw std::invalid_argument("provider API URL is required");
    if (ProviderUsesChatGPTPlan(profile.kind) && profile.base_url != "https://api.openai.com/v1")
        throw std::invalid_argument("ChatGPT plan access must use https://api.openai.com/v1");
}

std::vector<ProviderProfile> DefaultProviderProfiles() {
    return {
        {
            "chatgpt", "ChatGPT Plus / Pro", ProviderKind::ChatGPTPlan,
            "https://api.openai.com/v1", {}, {}, {"Sign in to load models"}, "Sign in to load models",
        },
        {
            "openai", "OpenAI", ProviderKind::OpenAI,
            "https://api.openai.com/v1", "OPENAI_API_KEY", {},
            {"gpt-5.6-sol", "gpt-5.6-terra", "gpt-5.6-luna", "gpt-5.5", "gpt-4.1"},
            "gpt-5.6-sol",
        },
        {
            "anthropic", "Anthropic", ProviderKind::AnthropicCompatibility,
            "https://api.anthropic.com/v1", "ANTHROPIC_API_KEY", {},
            {"claude-opus-5-5", "claude-sonnet-5", "claude-haiku-4-5-20251001"},
            "claude-opus-5-5",
        },
        {
            "custom", "Custom / OpenRouter", ProviderKind::OpenAICompatible,
            "https://openrouter.ai/api/v1", "KAIRO_API_KEY", {},
            {"qwen/qwen3.8-27b", "openai/gpt-oss-120b", "openai/gpt-oss-20b"},
            "qwen/qwen3.8-27b",
        },
    };
}

std::shared_ptr<Provider> CreateProvider(const ProviderProfile& profile,
                                         long timeout_seconds,
                                         const std::string& chatgpt_credential_file) {
    ValidateProviderProfile(profile);
    if (ProviderUsesChatGPTPlan(profile.kind))
        return std::make_shared<ChatGPTProvider>(ChatGPTConfig{chatgpt_credential_file, timeout_seconds});
    return std::make_shared<OpenAIProvider>(OpenAIConfig{
        profile.base_url,
        profile.api_key_environment,
        timeout_seconds,
        profile.api_key,
        profile.kind == ProviderKind::AnthropicCompatibility,
    });
}

}  // namespace kairo
