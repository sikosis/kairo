#include "kairo/agent_engine.h"
#include "kairo/openai_provider.h"
#include "kairo/sse_decoder.h"
#include "json.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

void Check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        path_ = fs::temp_directory_path() / ("kairo-tests-" + std::to_string(::getpid()));
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~TemporaryDirectory() { std::error_code ignored; fs::remove_all(path_, ignored); }
    const fs::path& path() const { return path_; }
private:
    fs::path path_;
};

class ToolThenAnswerProvider : public kairo::Provider {
public:
    explicit ToolThenAnswerProvider(std::string path = "generated.txt") : path_(std::move(path)) {}
    kairo::ProviderResponse Complete(const kairo::ProviderRequest& request,
                                     const kairo::EventSink& events,
                                     const kairo::CancellationToken&) override {
        if (!request.messages.empty() && request.messages.back().role == kairo::Role::Tool) {
            std::string text = "final: " + request.messages.back().content;
            if (events) {
                events({kairo::EventType::TextDelta, {}, {}, "final: "});
                events({kairo::EventType::TextDelta, {}, {}, request.messages.back().content});
            }
            return {text, {}};
        }
        std::string arguments;
        arguments += "{\"path\":\"";
        arguments += path_;
        arguments += "\",\"content\":\"hello";
        arguments += " from fragments\\n\"}";
        return {{}, {{"call-1", "write_file", arguments}}};
    }
private:
    std::string path_;
};

void TestSseFragmentation() {
    std::vector<std::string> events;
    kairo::SseDecoder decoder([&](const std::string& value) { events.push_back(value); });
    const std::string input = "data: {\"a\":1}\r\n\r\ndata: first\ndata: second\n\n";
    for (char c : input) decoder.Feed(&c, 1);
    decoder.Finish();
    Check(events.size() == 2, "SSE event count");
    Check(events[0] == "{\"a\":1}", "fragmented SSE JSON");
    Check(events[1] == "first\nsecond", "multiline SSE data");
}

void TestInputSafetyLimits() {
    bool rejected = false;
    try {
        kairo::SseDecoder decoder([](const std::string&) {});
        std::string oversized(1024 * 1024 + 1, 'x');
        decoder.Feed(oversized.data(), oversized.size());
    } catch (const std::runtime_error&) { rejected = true; }
    Check(rejected, "oversized SSE line rejected");

    std::string nested(130, '[');
    nested += "null";
    nested.append(130, ']');
    rejected = false;
    try { (void)kairo::json::Parse(nested); }
    catch (const std::runtime_error&) { rejected = true; }
    Check(rejected, "deeply nested JSON rejected");
}

void TestProviderUrlPolicy() {
    (void)kairo::OpenAIProvider({"https://api.example.test/v1", "KAIRO_API_KEY", 30, {}});
    (void)kairo::OpenAIProvider({"http://localhost:8080/v1", "KAIRO_API_KEY", 30, {}});
    bool rejected = false;
    try { (void)kairo::OpenAIProvider({"http://example.test/v1", "KAIRO_API_KEY", 30, {}}); }
    catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, "remote plaintext provider URL rejected");
    rejected = false;
    try { (void)kairo::OpenAIProvider({"https://user:secret@example.test/v1", "KAIRO_API_KEY", 30, {}}); }
    catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, "provider URL credentials rejected");
}

void TestJsonUnicode() {
    auto value = kairo::json::Parse(R"("\ud83d\ude00")");
    Check(value.string == "\xf0\x9f\x98\x80", "JSON surrogate pair decoding");
}

void TestWorkspacePolicy() {
    TemporaryDirectory temporary;
    fs::create_directories(temporary.path() / "project");
    fs::create_directories(temporary.path() / "sessions");
    std::ofstream(temporary.path() / "project" / "inside.txt") << "needle\n";
    std::ofstream binary(temporary.path() / "project" / "program", std::ios::binary);
    binary.write("\x7f" "ELF\0binary", 11);
    binary.close();
    std::ofstream(temporary.path() / "outside.txt") << "secret\n";
    fs::create_symlink(temporary.path() / "outside.txt", temporary.path() / "project" / "outside-link.txt");
    kairo::Workspace workspace(temporary.path() / "project", 4096);
    kairo::CancellationToken cancellation;

    auto read = workspace.Execute({"r", "read_file", R"({"path":"inside.txt"})"}, {}, cancellation);
    Check(read.ok && read.output == "needle\n", "in-project read");
    auto binary_read = workspace.Execute({"rb", "read_file", R"({"path":"program"})"}, {}, cancellation);
    Check(!binary_read.ok && binary_read.output.find("appears to be binary") != std::string::npos,
          "binary file rejected by text reader");
    auto search = workspace.Execute({"s", "search_files", R"({"query":"needle","path":"."})"}, {}, cancellation);
    Check(search.ok && search.output.find("inside.txt:1") != std::string::npos, "in-project search");
    auto escape = workspace.Execute({"e", "read_file", R"({"path":"../outside.txt"})"}, {}, cancellation);
    Check(!escape.ok && escape.output.find("escapes project") != std::string::npos, "path escape rejected");
    auto symlink_escape = workspace.Execute({"e2", "read_file", R"({"path":"outside-link.txt"})"}, {}, cancellation);
    Check(!symlink_escape.ok && symlink_escape.output.find("escapes project") != std::string::npos, "symlink escape rejected");
    auto shell_path = workspace.Execute({"e3", "search_files", R"({"query":"needle","path":"$HOME"})"}, {}, cancellation);
    Check(!shell_path.ok && shell_path.output.find("use '.' for the project root") != std::string::npos,
          "shell-style path rejected with corrective guidance");

    auto denied_write = workspace.Execute({"w", "write_file", R"({"path":"denied.txt","content":"no"})"},
        [](const kairo::ProposedAction&) { return false; }, cancellation);
    Check(denied_write.denied && !fs::exists(temporary.path() / "project" / "denied.txt"), "denied write has no side effect");
    auto allowed_write = workspace.Execute({"w", "write_file", R"({"path":"allowed.txt","content":"yes"})"},
        [](const kairo::ProposedAction& action) {
            return action.preview.find("allowed.txt") != std::string::npos;
        }, cancellation);
    Check(allowed_write.ok && Read(temporary.path() / "project" / "allowed.txt") == "yes", "approved write");

    auto denied_shell = workspace.Execute({"x", "run_shell", R"({"command":"touch shell-side-effect","working_directory":"."})"},
        [](const kairo::ProposedAction&) { return false; }, cancellation);
    Check(denied_shell.denied && !fs::exists(temporary.path() / "project" / "shell-side-effect"), "denied shell has no side effect");
    auto allowed_shell = workspace.Execute({"x2", "run_shell", R"({"command":"printf shell-ok","working_directory":"."})"},
        [](const kairo::ProposedAction& action) {
            return action.target == "printf shell-ok" && !action.working_directory.empty();
        }, cancellation);
    Check(allowed_shell.ok && allowed_shell.output.find("shell-ok") != std::string::npos, "approved shell command");
    auto closed_stdin = workspace.Execute({"x3", "run_shell", R"({"command":"if read value; then exit 9; else printf stdin-closed; fi","working_directory":"."})"},
        [](const kairo::ProposedAction&) { return true; }, cancellation);
    Check(closed_stdin.ok && closed_stdin.output.find("stdin-closed") != std::string::npos,
          "shell command cannot consume application stdin");

    ::setenv("KAIRO_PRIVATE_VALUE", "must-not-reach-shell", 1);
    kairo::Workspace sanitized_workspace(temporary.path() / "project", 4096,
                                         {"KAIRO_PRIVATE_VALUE"});
    auto sanitized = sanitized_workspace.Execute({"x4", "run_shell",
        R"({"command":"test -z \"$KAIRO_PRIVATE_VALUE\"","working_directory":"."})"},
        [](const kairo::ProposedAction&) { return true; }, cancellation);
    ::unsetenv("KAIRO_PRIVATE_VALUE");
    Check(sanitized.ok, "configured credential environment variable removed from shell");
}

void TestAgentApprovalAndResume() {
    TemporaryDirectory temporary;
    fs::path project = temporary.path() / "project";
    fs::path sessions = temporary.path() / "sessions";
    fs::create_directories(project);
    kairo::SessionStore store(sessions);
    kairo::Session session = store.Create(project, "fake-model");
    auto provider = std::make_shared<ToolThenAnswerProvider>();
    kairo::AgentEngine engine(provider, kairo::Workspace(project, 4096), store);
    std::vector<kairo::EventType> event_types;
    auto sink = [&](const kairo::EngineEvent& event) {
        Check(event.session_id == session.id, "event session id");
        event_types.push_back(event.type);
    };
    std::string result = engine.Run(session, "make a file",
        [](const kairo::ProposedAction&) { return true; }, sink, kairo::CancellationToken{});
    Check(result.find("Wrote") != std::string::npos, "agent final response");
    Check(Read(project / "generated.txt") == "hello from fragments\n", "fragmented tool arguments assembled");
    Check(!event_types.empty() && event_types.back() == kairo::EventType::Completed, "agent completion event");

    kairo::Session resumed = store.Load(session.id);
    Check(resumed.messages.size() == 4, "session resume message count");
    Check(resumed.project == fs::canonical(project).string(), "session project persisted");
    struct stat metadata{};
    Check(::stat((sessions / (session.id + ".json")).c_str(), &metadata) == 0, "session file exists");
    Check((metadata.st_mode & 077) == 0, "session file permissions are restrictive");
}

void TestAgentDenialAndCancellation() {
    TemporaryDirectory temporary;
    fs::path project = temporary.path() / "project";
    fs::create_directories(project);
    kairo::SessionStore store(temporary.path() / "sessions");
    kairo::Session session = store.Create(project, "fake-model");
    kairo::AgentEngine engine(std::make_shared<ToolThenAnswerProvider>("must-not-exist.txt"),
                              kairo::Workspace(project, 4096), store);
    std::string result = engine.Run(session, "deny this",
        [](const kairo::ProposedAction&) { return false; }, {}, kairo::CancellationToken{});
    Check(result.find("denied") != std::string::npos, "denial becomes a tool result");
    Check(!fs::exists(project / "must-not-exist.txt"), "denied engine write has no side effect");

    kairo::Session cancelled_session = store.Create(project, "fake-model");
    kairo::CancellationToken cancellation;
    cancellation.Cancel();
    bool cancelled_event = false;
    std::string cancelled = engine.Run(cancelled_session, "stop", {},
        [&](const kairo::EngineEvent& event) { cancelled_event |= event.type == kairo::EventType::Cancelled; }, cancellation);
    Check(cancelled.empty() && cancelled_event, "pre-cancelled run stops cleanly");
}

}  // namespace

int main() {
    try {
        TestSseFragmentation();
        TestInputSafetyLimits();
        TestProviderUrlPolicy();
        TestJsonUnicode();
        TestWorkspacePolicy();
        TestAgentApprovalAndResume();
        TestAgentDenialAndCancellation();
        std::cout << "All Kairo tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
