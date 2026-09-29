#pragma once

#include "kairo/approval.h"
#include "kairo/cancellation.h"
#include "kairo/events.h"
#include "kairo/model.h"
#include "kairo/provider.h"
#include "kairo/session_store.h"
#include "kairo/workspace.h"

#include <memory>
#include <string>

namespace kairo {

class AgentEngine {
public:
    AgentEngine(std::shared_ptr<Provider> provider, Workspace workspace,
                SessionStore sessions, Limits limits = {});

    std::string Run(Session& session, const std::string& prompt,
                    const ApprovalHandler& approval, const EventSink& events,
                    const CancellationToken& cancellation) const;

private:
    std::shared_ptr<Provider> provider_;
    Workspace workspace_;
    SessionStore sessions_;
    Limits limits_;
};

}  // namespace kairo
