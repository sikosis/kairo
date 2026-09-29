#include "kairo/events.h"

namespace kairo {

const char* EventTypeName(EventType type) {
    switch (type) {
        case EventType::Started: return "started";
        case EventType::TextDelta: return "text_delta";
        case EventType::ApprovalRequested: return "approval_requested";
        case EventType::ToolStarted: return "tool_started";
        case EventType::ToolFinished: return "tool_finished";
        case EventType::ToolDenied: return "tool_denied";
        case EventType::SessionSaved: return "session_saved";
        case EventType::Completed: return "completed";
        case EventType::Cancelled: return "cancelled";
        case EventType::Error: return "error";
    }
    return "unknown";
}

}  // namespace kairo
