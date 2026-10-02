#pragma once

#include "kairo/approval.h"
#include "kairo/cancellation.h"
#include "kairo/events.h"
#include "kairo/session_store.h"

#include <filesystem>
#include <string>

namespace kairo {

struct CodexConfig {
    std::string executable = "codex";
};

class CodexRunner {
public:
    explicit CodexRunner(CodexConfig config);

    std::string Run(Session& session, const std::filesystem::path& project,
                    const std::string& prompt, bool require_approvals,
                    const ApprovalHandler& approval, const EventSink& events,
                    const CancellationToken& cancellation,
                    const SessionStore& sessions) const;

private:
    CodexConfig config_;
};

}  // namespace kairo
