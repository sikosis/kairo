#pragma once

#include "kairo/approval.h"
#include "kairo/cancellation.h"
#include "kairo/model.h"

#include <filesystem>
#include <string>
#include <vector>

namespace kairo {

struct ToolResult {
    bool ok = false;
    bool denied = false;
    std::string output;
};

class Workspace {
public:
    Workspace(std::filesystem::path root, std::size_t output_limit,
              std::vector<std::string> sensitive_environment_variables = {});

    const std::filesystem::path& Root() const { return root_; }
    ToolResult Execute(const ToolCall& call, const ApprovalHandler& approval,
                       const CancellationToken& cancellation) const;

private:
    std::filesystem::path ResolveInside(const std::string& relative, bool allow_missing) const;
    ToolResult ReadFile(const std::string& arguments) const;
    ToolResult SearchFiles(const std::string& arguments) const;
    ToolResult WriteFile(const ToolCall& call, const ApprovalHandler& approval) const;
    ToolResult RunShell(const ToolCall& call, const ApprovalHandler& approval,
                        const CancellationToken& cancellation) const;
    std::string Limit(std::string value) const;

    std::filesystem::path root_;
    std::size_t output_limit_;
    std::vector<std::string> sensitive_environment_variables_;
};

}  // namespace kairo
