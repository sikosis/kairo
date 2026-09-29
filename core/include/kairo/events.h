#pragma once

#include <functional>
#include <string>

namespace kairo {

enum class EventType {
    Started,
    TextDelta,
    ApprovalRequested,
    ToolStarted,
    ToolFinished,
    ToolDenied,
    SessionSaved,
    Completed,
    Cancelled,
    Error
};

struct EngineEvent {
    EventType type = EventType::Started;
    std::string session_id;
    std::string tool_call_id;
    std::string text;
};

using EventSink = std::function<void(const EngineEvent&)>;

const char* EventTypeName(EventType type);

}  // namespace kairo
