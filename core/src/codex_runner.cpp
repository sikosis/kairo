#include "kairo/codex_runner.h"

#include "json.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
extern char** environ;

namespace kairo {
namespace {

constexpr std::size_t kMaximumProtocolLine = 8 * 1024 * 1024;
constexpr std::size_t kMaximumDiagnostics = 64 * 1024;
constexpr std::size_t kMaximumPromptBytes = 512 * 1024;
constexpr std::size_t kMaximumResponseBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaximumToolOutputBytes = 64 * 1024;
constexpr const char* kDefaultModel = "Use Codex default";

bool SensitiveEnvironmentName(const std::string& name) {
    std::string upper;
    upper.reserve(name.size());
    for (unsigned char character : name)
        upper += static_cast<char>(std::toupper(character));
    constexpr const char* markers[] = {
        "API_KEY", "TOKEN", "SECRET", "PASSWORD", "PASSWD", "CREDENTIAL", "AUTH"
    };
    for (const char* marker : markers)
        if (upper.find(marker) != std::string::npos) return true;
    return upper == "SSH_AUTH_SOCK" || upper == "GIT_ASKPASS" || upper == "SSH_ASKPASS";
}

std::vector<std::string> SanitizedEnvironment() {
    std::vector<std::string> result;
    for (char** item = environ; item && *item; ++item) {
        std::string entry(*item);
        const std::size_t equals = entry.find('=');
        if (!SensitiveEnvironmentName(entry.substr(0, equals))) result.push_back(std::move(entry));
    }
    return result;
}

fs::path ResolveExecutable(const std::string& command) {
    if (command.empty()) throw std::invalid_argument("Codex executable is empty");
    if (command.find('/') != std::string::npos) {
        std::error_code error;
        fs::path path = fs::canonical(command, error);
        if (error || !fs::is_regular_file(path) || ::access(path.c_str(), X_OK) != 0)
            throw std::runtime_error("Codex executable is not available: " + command);
        return path;
    }
    const char* raw_path = std::getenv("PATH");
    std::string path = raw_path ? raw_path : "/bin:/boot/system/bin";
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t end = path.find(':', start);
        fs::path candidate = path.substr(start, end == std::string::npos
            ? std::string::npos : end - start);
        if (candidate.empty()) candidate = ".";
        candidate /= command;
        if (fs::is_regular_file(candidate) && ::access(candidate.c_str(), X_OK) == 0)
            return fs::canonical(candidate);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    throw std::runtime_error("Cannot find the Codex executable. Install Codex, run 'codex login', "
                             "then set its path in Provider Settings.");
}

json::Value Object(std::initializer_list<std::pair<const std::string, json::Value>> values) {
    return json::Value::Object(std::map<std::string, json::Value>(values));
}

void WriteAll(int descriptor, const std::string& data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t written = ::write(descriptor, data.data() + offset, data.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) throw std::runtime_error("lost connection to codex app-server");
        offset += static_cast<std::size_t>(written);
    }
}

void Send(int descriptor, const json::Value& message) {
    WriteAll(descriptor, json::Encode(message) + "\n");
}

std::string RequestId(const json::Value& value) {
    const json::Value* id = value.Find("id");
    if (!id) return {};
    if (id->type == json::Value::Type::String) return json::Escape(id->string);
    if (id->type == json::Value::Type::Number) return json::Encode(*id);
    return {};
}

std::string ErrorMessage(const json::Value& value) {
    const json::Value* error = value.Find("error");
    if (!error || error->type != json::Value::Type::Object) return {};
    return error->GetString("message", "codex app-server request failed");
}

void Emit(const EventSink& events, EventType type, const Session& session,
          const std::string& text = {}, const std::string& call_id = {}) {
    if (events) events({type, session.id, call_id, text});
}

std::string ApprovalPreview(const std::string& method, const json::Value& params) {
    if (method == "item/commandExecution/requestApproval") {
        std::string preview = "Command: " + params.GetString("command", "(not supplied)");
        const std::string cwd = params.GetString("cwd");
        const std::string reason = params.GetString("reason");
        if (!cwd.empty()) preview += "\nWorking directory: " + cwd;
        if (!reason.empty()) preview += "\nReason: " + reason;
        return preview;
    }
    std::string preview = "Codex wants to change files in the selected project.";
    const std::string reason = params.GetString("reason");
    const std::string root = params.GetString("grantRoot");
    if (!root.empty()) preview += "\nRequested root: " + root;
    if (!reason.empty()) preview += "\nReason: " + reason;
    return preview;
}

std::string Truncated(std::string value, std::size_t limit) {
    if (value.size() <= limit) return value;
    value.resize(limit);
    value += "\n...[output truncated by Kairo]";
    return value;
}

}  // namespace

CodexRunner::CodexRunner(CodexConfig config) : config_(std::move(config)) {}

std::string CodexRunner::Run(Session& session, const fs::path& project,
                             const std::string& prompt, bool require_approvals,
                             const ApprovalHandler& approval, const EventSink& events,
                             const CancellationToken& cancellation,
                             const SessionStore& sessions) const {
    if (prompt.size() > kMaximumPromptBytes)
        throw std::runtime_error("prompt exceeds Kairo's 512 KiB safety limit");
    const fs::path executable = ResolveExecutable(config_.executable);
    const fs::path cwd = fs::canonical(project);
    int input_pipe[2];
    int output_pipe[2];
    int error_pipe[2];
    if (::pipe(input_pipe) != 0)
        throw std::runtime_error("cannot create input pipe for codex app-server");
    if (::pipe(output_pipe) != 0) {
        ::close(input_pipe[0]); ::close(input_pipe[1]);
        throw std::runtime_error("cannot create output pipe for codex app-server");
    }
    if (::pipe(error_pipe) != 0) {
        ::close(input_pipe[0]); ::close(input_pipe[1]);
        ::close(output_pipe[0]); ::close(output_pipe[1]);
        throw std::runtime_error("cannot create diagnostic pipe for codex app-server");
    }

    std::vector<std::string> environment = SanitizedEnvironment();
    std::vector<char*> environment_pointers;
    for (std::string& entry : environment) environment_pointers.push_back(entry.data());
    environment_pointers.push_back(nullptr);

    pid_t child = ::fork();
    if (child < 0) {
        ::close(input_pipe[0]); ::close(input_pipe[1]);
        ::close(output_pipe[0]); ::close(output_pipe[1]);
        ::close(error_pipe[0]); ::close(error_pipe[1]);
        throw std::runtime_error("cannot start codex app-server");
    }
    if (child == 0) {
        ::setpgid(0, 0);
        ::dup2(input_pipe[0], STDIN_FILENO);
        ::dup2(output_pipe[1], STDOUT_FILENO);
        ::dup2(error_pipe[1], STDERR_FILENO);
        ::close(input_pipe[0]); ::close(input_pipe[1]);
        ::close(output_pipe[0]); ::close(output_pipe[1]);
        ::close(error_pipe[0]); ::close(error_pipe[1]);
        char* arguments[] = {const_cast<char*>(executable.c_str()),
                             const_cast<char*>("app-server"),
                             const_cast<char*>("--listen"),
                             const_cast<char*>("stdio://"), nullptr};
        ::execve(executable.c_str(), arguments, environment_pointers.data());
        _exit(127);
    }
    ::setpgid(child, child);
    ::close(input_pipe[0]);
    ::close(output_pipe[1]);
    ::close(error_pipe[1]);
    ::fcntl(output_pipe[0], F_SETFL, ::fcntl(output_pipe[0], F_GETFL) | O_NONBLOCK);
    ::fcntl(error_pipe[0], F_SETFL, ::fcntl(error_pipe[0], F_GETFL) | O_NONBLOCK);

    bool child_reaped = false;
    bool descriptors_closed = false;
    auto stop_child = [&]() {
        if (descriptors_closed) return;
        ::close(input_pipe[1]);
        int status = 0;
        if (!child_reaped) {
            ::kill(-child, SIGTERM);
            for (int attempt = 0; attempt < 50; ++attempt) {
                const pid_t result = ::waitpid(child, &status, WNOHANG);
                if (result == child || (result < 0 && errno == ECHILD)) {
                    child_reaped = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!child_reaped) {
                ::kill(-child, SIGKILL);
                while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
                child_reaped = true;
            }
        }
        ::close(output_pipe[0]);
        ::close(error_pipe[0]);
        descriptors_closed = true;
    };

    Emit(events, EventType::Started, session);
    try {
        Send(input_pipe[1], Object({
            {"id", json::Value::Number(1)}, {"method", json::Value::String("initialize")},
            {"params", Object({{"clientInfo", Object({
                {"name", json::Value::String("kairo")},
                {"title", json::Value::String("Kairo")},
                {"version", json::Value::String("0.1")},
            })}})},
        }));
        Send(input_pipe[1], Object({{"method", json::Value::String("initialized")},
                                     {"params", Object({})}}));

        std::map<std::string, json::Value> thread_params{
            {"cwd", json::Value::String(cwd.string())},
            {"approvalPolicy", json::Value::String(require_approvals ? "on-request" : "never")},
            {"approvalsReviewer", json::Value::String("user")},
            {"sandbox", json::Value::String(require_approvals ? "read-only" : "workspace-write")},
            {"developerInstructions", json::Value::String(
                "You are running inside Kairo on Haiku. Stay inside the selected project. "
                "For native Haiku GUI apps, use C++ with BApplication and BWindow, compile with "
                "g++, and link with -lbe. Do not probe for obsolete BeOS tools such as bcc, bchk, "
                "bimg, or getbeospath.")},
        };
        if (session.model != kDefaultModel && !session.model.empty())
            thread_params["model"] = json::Value::String(session.model);
        const bool resume = !session.backend_thread_id.empty();
        if (resume) thread_params["threadId"] = json::Value::String(session.backend_thread_id);
        Send(input_pipe[1], Object({
            {"id", json::Value::Number(2)},
            {"method", json::Value::String(resume ? "thread/resume" : "thread/start")},
            {"params", json::Value::Object(std::move(thread_params))},
        }));

        bool turn_started = false;
        bool finished = false;
        std::string output_buffer;
        std::string diagnostics;
        std::string response_text;
        while (!finished) {
            if (cancellation.IsCancelled()) {
                stop_child();
                Emit(events, EventType::Cancelled, session);
                return {};
            }
            pollfd descriptors[] = {{output_pipe[0], POLLIN, 0}, {error_pipe[0], POLLIN, 0}};
            (void)::poll(descriptors, 2, 100);
            std::array<char, 4096> buffer{};
            ssize_t count;
            while ((count = ::read(error_pipe[0], buffer.data(), buffer.size())) > 0)
                if (diagnostics.size() < kMaximumDiagnostics)
                    diagnostics.append(buffer.data(), std::min<std::size_t>(count,
                        kMaximumDiagnostics - diagnostics.size()));
            while ((count = ::read(output_pipe[0], buffer.data(), buffer.size())) > 0) {
                output_buffer.append(buffer.data(), static_cast<std::size_t>(count));
                if (output_buffer.size() > kMaximumProtocolLine)
                    throw std::runtime_error("codex app-server sent an oversized protocol message");
            }

            std::size_t newline;
            while ((newline = output_buffer.find('\n')) != std::string::npos) {
                std::string line = output_buffer.substr(0, newline);
                output_buffer.erase(0, newline + 1);
                if (line.empty()) continue;
                json::Value message = json::Parse(line);
                if (const std::string error = ErrorMessage(message); !error.empty())
                    throw std::runtime_error("codex app-server: " + error);
                const json::Value* id = message.Find("id");
                const json::Value* result = message.Find("result");
                if (!turn_started && id && id->type == json::Value::Type::Number
                    && static_cast<int>(id->number) == 2 && result) {
                    const json::Value* thread = result->Find("thread");
                    if (!thread) throw std::runtime_error("codex app-server returned no thread");
                    session.backend_thread_id = thread->GetString("id");
                    if (session.backend_thread_id.empty())
                        throw std::runtime_error("codex app-server returned an empty thread ID");
                    Send(input_pipe[1], Object({
                        {"id", json::Value::Number(3)},
                        {"method", json::Value::String("turn/start")},
                        {"params", Object({
                            {"threadId", json::Value::String(session.backend_thread_id)},
                            {"input", json::Value::Array({Object({
                                {"type", json::Value::String("text")},
                                {"text", json::Value::String(prompt)},
                            })})},
                        })},
                    }));
                    turn_started = true;
                    continue;
                }

                const std::string method = message.GetString("method");
                const json::Value* params = message.Find("params");
                if ((method == "item/commandExecution/requestApproval" ||
                     method == "item/fileChange/requestApproval") && params) {
                    const std::string preview = ApprovalPreview(method, *params);
                    const std::string call_id = params->GetString("itemId");
                    Emit(events, EventType::ApprovalRequested, session, preview, call_id);
                    ProposedAction action{
                        method == "item/commandExecution/requestApproval"
                            ? ActionKind::ShellCommand : ActionKind::WriteFile,
                        call_id, params->GetString("command"), params->GetString("cwd"), preview};
                    const bool accepted = !require_approvals || (approval && approval(action));
                    const std::string request_id = RequestId(message);
                    if (request_id.empty()) throw std::runtime_error("Codex approval had no request ID");
                    WriteAll(input_pipe[1], "{\"id\":" + request_id
                        + ",\"result\":{\"decision\":\""
                        + (accepted ? "accept" : "decline") + "\"}}\n");
                    if (!accepted) Emit(events, EventType::ToolDenied, session,
                                        "Action denied by user.", call_id);
                    continue;
                }
                if (method == "item/agentMessage/delta" && params) {
                    const std::string delta = params->GetString("delta");
                    if (delta.size() > kMaximumResponseBytes
                        || response_text.size() > kMaximumResponseBytes - delta.size())
                        throw std::runtime_error("Codex response exceeds Kairo's 8 MiB safety limit");
                    response_text += delta;
                    Emit(events, EventType::TextDelta, session, delta);
                    continue;
                }
                if (method == "item/started" && params) {
                    const json::Value* item = params->Find("item");
                    if (item && item->GetString("type") == "commandExecution")
                        Emit(events, EventType::ToolStarted, session,
                             item->GetString("command", "Codex command"), item->GetString("id"));
                    continue;
                }
                if (method == "item/completed" && params) {
                    const json::Value* item = params->Find("item");
                    if (item && item->GetString("type") == "commandExecution")
                        Emit(events, EventType::ToolFinished, session,
                             Truncated(item->GetString("aggregatedOutput"),
                                       kMaximumToolOutputBytes), item->GetString("id"));
                    continue;
                }
                if (method == "turn/completed" && params) {
                    const json::Value* turn = params->Find("turn");
                    const std::string status = turn ? turn->GetString("status") : "failed";
                    if (status == "failed") {
                        const json::Value* error = turn ? turn->Find("error") : nullptr;
                        throw std::runtime_error(error ? error->GetString("message", "Codex turn failed")
                                                       : "Codex turn failed");
                    }
                    if (status == "interrupted") {
                        stop_child();
                        Emit(events, EventType::Cancelled, session);
                        return {};
                    }
                    finished = status == "completed";
                }
            }

            int status = 0;
            const pid_t waited = ::waitpid(child, &status, WNOHANG);
            if (waited == child && !finished) {
                child_reaped = true;
                std::string detail = diagnostics.empty() ? "" : ": " + diagnostics;
                throw std::runtime_error("codex app-server exited before completing the turn" + detail);
            }
            if (waited == child) child_reaped = true;
        }

        session.messages.push_back({Role::User, prompt, {}, {}});
        session.messages.push_back({Role::Assistant, response_text, {}, {}});
        sessions.Save(session);
        Emit(events, EventType::SessionSaved, session, sessions.Directory().string());
        Emit(events, EventType::Completed, session, response_text);
        stop_child();
        return response_text;
    } catch (const std::exception& error) {
        stop_child();
        Emit(events, EventType::Error, session, error.what());
        throw;
    }
}

}  // namespace kairo
