#pragma once

#include "kairo/provider.h"

#include <filesystem>

namespace kairo {

struct ChatGPTConfig {
    std::filesystem::path credential_file;
    long timeout_seconds = 120;
};

class ChatGPTProvider : public Provider {
public:
    explicit ChatGPTProvider(ChatGPTConfig config);
    ProviderResponse Complete(const ProviderRequest& request, const EventSink& events,
                              const CancellationToken& cancellation) override;

private:
    ChatGPTConfig config_;
};

}  // namespace kairo
