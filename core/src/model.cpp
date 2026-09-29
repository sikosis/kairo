#include "kairo/model.h"

#include <stdexcept>

namespace kairo {

std::string RoleName(Role role) {
    switch (role) {
        case Role::System: return "system";
        case Role::User: return "user";
        case Role::Assistant: return "assistant";
        case Role::Tool: return "tool";
    }
    return "user";
}

Role ParseRole(const std::string& value) {
    if (value == "system") return Role::System;
    if (value == "user") return Role::User;
    if (value == "assistant") return Role::Assistant;
    if (value == "tool") return Role::Tool;
    throw std::runtime_error("unknown role: " + value);
}

}  // namespace kairo
