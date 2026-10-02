#include "kairo/provider_profile.h"

#include "kairo/openai_provider.h"

#include <stdexcept>

namespace kairo {

const char* ProviderKindName(ProviderKind kind) {
    switch (kind) {
        case ProviderKind::OpenAI: return "OpenAI API";
        case ProviderKind::AnthropicCompatibility: return "Anthropic API";
        case ProviderKind::OpenAICompatible: return "OpenAI-compatible";
        case ProviderKind::CodexChatGPT: return "ChatGPT plan (Codex)";
    }
    return "OpenAI-compatible";
}

std::string ProviderKindId(ProviderKind kind) {
    switch (kind) {
        case ProviderKind::OpenAI: return "openai";
        case ProviderKind::AnthropicCompatibility: return "anthropic";
        case ProviderKind::OpenAICompatible: return "openai-compatible";
        case ProviderKind::CodexChatGPT: return "codex-chatgpt";
    }
    return "openai-compatible";
}

ProviderKind ParseProviderKind(const std::string& value) {
    if (value == "openai") return ProviderKind::OpenAI;
    if (value == "anthropic") return ProviderKind::AnthropicCompatibility;
    if (value == "openai-compatible") return ProviderKind::OpenAICompatible;
    if (value == "codex-chatgpt") return ProviderKind::CodexChatGPT;
    throw std::runtime_error("unknown provider kind: " + value);
}

bool ProviderUsesApiKey(ProviderKind kind) {
    return kind != ProviderKind::CodexChatGPT;
}

bool ProviderUsesCodexAppServer(ProviderKind kind) {
    return kind == ProviderKind::CodexChatGPT;
}

void ValidateProviderProfile(const ProviderProfile& profile) {
    if (profile.name.empty()) throw std::invalid_argument("provider name is required");
    if (profile.models.empty()) throw std::invalid_argument("provider has no configured models");
    if (ProviderUsesCodexAppServer(profile.kind)) {
        if (profile.base_url.empty())
            throw std::invalid_argument("Codex executable path or command is required");
        return;
    }
    if (profile.base_url.empty()) throw std::invalid_argument("provider API URL is required");
}

std::vector<ProviderProfile> DefaultProviderProfiles() {
    return {
        {
            "codex", "Codex (ChatGPT plan)", ProviderKind::CodexChatGPT,
            "codex", {}, {},
            {"Use Codex default"}, "Use Codex default",
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
                                         long timeout_seconds) {
    ValidateProviderProfile(profile);
    if (ProviderUsesCodexAppServer(profile.kind))
        throw std::invalid_argument("Codex profiles run through codex app-server, not the API provider factory");
    return std::make_shared<OpenAIProvider>(OpenAIConfig{
        profile.base_url,
        profile.api_key_environment,
        timeout_seconds,
        profile.api_key,
        profile.kind == ProviderKind::AnthropicCompatibility,
    });
}

}  // namespace kairo
