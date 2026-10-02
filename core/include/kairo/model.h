#pragma once

#include <cstddef>
#include <string>
#include <utility>
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
    std::vector<std::string> provider_items;

    Message() = default;
    Message(Role message_role, std::string message_content,
            std::string message_tool_call_id = {},
            std::vector<ToolCall> message_tool_calls = {},
            std::vector<std::string> message_provider_items = {})
        : role(message_role), content(std::move(message_content)),
          tool_call_id(std::move(message_tool_call_id)),
          tool_calls(std::move(message_tool_calls)),
          provider_items(std::move(message_provider_items)) {}
};

struct ProviderResponse {
    std::string text;
    std::vector<ToolCall> tool_calls;
    std::vector<std::string> provider_items;

    ProviderResponse() = default;
    ProviderResponse(std::string response_text, std::vector<ToolCall> response_tool_calls,
                     std::vector<std::string> response_provider_items = {})
        : text(std::move(response_text)), tool_calls(std::move(response_tool_calls)),
          provider_items(std::move(response_provider_items)) {}
};

struct Limits {
    std::size_t max_iterations = 12;
    std::size_t max_request_bytes = 512 * 1024;
    std::size_t max_tool_output_bytes = 64 * 1024;
};

std::string RoleName(Role role);
Role ParseRole(const std::string& value);

}  // namespace kairo
