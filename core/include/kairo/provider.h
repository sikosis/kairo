#pragma once

#include "kairo/cancellation.h"
#include "kairo/events.h"
#include "kairo/model.h"

#include <string>
#include <vector>

namespace kairo {

struct ProviderRequest {
    std::string model;
    std::vector<Message> messages;
};

class Provider {
public:
    virtual ~Provider() = default;
    virtual ProviderResponse Complete(const ProviderRequest& request,
                                      const EventSink& events,
                                      const CancellationToken& cancellation) = 0;
};

}  // namespace kairo
