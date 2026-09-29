#include "kairo/agent_engine.h"

#include <stdexcept>

namespace kairo {

AgentEngine::AgentEngine(std::shared_ptr<Provider> provider, Workspace workspace,
                         SessionStore sessions, Limits limits)
    : provider_(std::move(provider)), workspace_(std::move(workspace)),
      sessions_(std::move(sessions)), limits_(limits) {
    if (!provider_) throw std::invalid_argument("provider is required");
}

std::string AgentEngine::Run(Session& session, const std::string& prompt,
                             const ApprovalHandler& approval, const EventSink& events,
                             const CancellationToken& cancellation) const {
    auto emit = [&](EventType type, const std::string& text = {}, const std::string& call_id = {}) {
        if (events) events({type, session.id, call_id, text});
    };
    emit(EventType::Started);
    if (!prompt.empty()) session.messages.push_back({Role::User, prompt, {}, {}});

    try {
        for (std::size_t iteration = 0; iteration < limits_.max_iterations; ++iteration) {
            if (cancellation.IsCancelled()) { emit(EventType::Cancelled); return {}; }
            std::size_t request_size = 0;
            auto add_size = [&](std::size_t amount) {
                if (amount > limits_.max_request_bytes ||
                    request_size > limits_.max_request_bytes - amount)
                    throw std::runtime_error("request exceeds configured size limit");
                request_size += amount;
            };
            for (const auto& message : session.messages) {
                add_size(message.content.size());
                add_size(message.tool_call_id.size());
                for (const auto& call : message.tool_calls)
                    { add_size(call.id.size()); add_size(call.name.size()); add_size(call.arguments.size()); }
            }

            EventSink provider_events = [&](const EngineEvent& incoming) {
                EngineEvent event = incoming;
                event.session_id = session.id;
                if (events) events(event);
            };
            ProviderResponse response = provider_->Complete({session.model, session.messages}, provider_events, cancellation);
            if (cancellation.IsCancelled()) { emit(EventType::Cancelled); return {}; }
            session.messages.push_back({Role::Assistant, response.text, {}, response.tool_calls});
            if (response.tool_calls.empty()) {
                sessions_.Save(session);
                emit(EventType::SessionSaved, sessions_.Directory().string());
                emit(EventType::Completed, response.text);
                return response.text;
            }

            for (const auto& call : response.tool_calls) {
                if (cancellation.IsCancelled()) { emit(EventType::Cancelled); return {}; }
                emit(EventType::ToolStarted, call.name, call.id);
                ApprovalHandler observed_approval = [&](const ProposedAction& action) {
                    emit(EventType::ApprovalRequested, action.preview, call.id);
                    return approval && approval(action);
                };
                ToolResult result = workspace_.Execute(call, observed_approval, cancellation);
                session.messages.push_back({Role::Tool, result.output, call.id, {}});
                emit(result.denied ? EventType::ToolDenied : EventType::ToolFinished,
                     result.output, call.id);
            }
            sessions_.Save(session);
            emit(EventType::SessionSaved, sessions_.Directory().string());
        }
        throw std::runtime_error("agent reached the iteration limit");
    } catch (const std::exception& error) {
        emit(EventType::Error, error.what());
        throw;
    }
}

}  // namespace kairo
