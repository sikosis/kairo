#pragma once

#include <functional>
#include <string>

namespace kairo {

enum class ActionKind { WriteFile, ShellCommand };

struct ProposedAction {
    ActionKind kind = ActionKind::WriteFile;
    std::string tool_call_id;
    std::string target;
    std::string working_directory;
    std::string preview;
};

using ApprovalHandler = std::function<bool(const ProposedAction&)>;

}  // namespace kairo
