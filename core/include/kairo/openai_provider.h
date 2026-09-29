#pragma once

#include "kairo/provider.h"

#include <string>

namespace kairo {

struct OpenAIConfig {
    std::string base_url;
    std::string api_key_environment = "KAIRO_API_KEY";
    long timeout_seconds = 120;
    std::string api_key;
};

class OpenAIProvider : public Provider {
public:
    explicit OpenAIProvider(OpenAIConfig config);
    ProviderResponse Complete(const ProviderRequest& request, const EventSink& events,
                              const CancellationToken& cancellation) override;

private:
    OpenAIConfig config_;
};

}  // namespace kairo
