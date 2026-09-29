#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace kairo {

enum class Role { System, User, Assistant, Tool };

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments;
};

struct Message {
    Role role = Role::User;
    std::string content;
    std::string tool_call_id;
    std::vector<ToolCall> tool_calls;
};

struct ProviderResponse {
    std::string text;
    std::vector<ToolCall> tool_calls;
};

struct Limits {
    std::size_t max_iterations = 12;
    std::size_t max_request_bytes = 512 * 1024;
    std::size_t max_tool_output_bytes = 64 * 1024;
};

std::string RoleName(Role role);
Role ParseRole(const std::string& value);

}  // namespace kairo
