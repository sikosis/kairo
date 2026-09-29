#include "kairo/agent_engine.h"
#include "kairo/openai_provider.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string TerminalSafe(const std::string& text) {
    std::string safe;
    safe.reserve(text.size());
    for (unsigned char character : text) {
        if (character == '\n' || character == '\r' || character == '\t' ||
            (character >= 0x20 && character != 0x7f))
            safe += static_cast<char>(character);
        else
            safe += "?";
    }
    return safe;
}

class DemoProvider : public kairo::Provider {
public:
    kairo::ProviderResponse Complete(const kairo::ProviderRequest& request,
                                     const kairo::EventSink& events,
                                     const kairo::CancellationToken&) override {
        if (!request.messages.empty() && request.messages.back().role == kairo::Role::Tool) {
            std::string text = "The tool result was recorded: " + request.messages.back().content;
            if (events) events({kairo::EventType::TextDelta, {}, {}, text});
            return {text, {}};
        }
        std::string prompt;
        for (auto it = request.messages.rbegin(); it != request.messages.rend(); ++it)
            if (it->role == kairo::Role::User) { prompt = it->content; break; }
        kairo::ToolCall call;
        call.id = "demo-call-1";
        if (prompt.find("shell") != std::string::npos) {
            call.name = "run_shell";
            call.arguments = R"({"command":"printf 'Kairo shell demo\\n'","working_directory":"."})";
        } else if (prompt.find("read") != std::string::npos) {
            call.name = "read_file";
            call.arguments = R"({"path":"README.md"})";
        } else {
            call.name = "write_file";
            call.arguments = R"({"path":"kairo-demo.txt","content":"Created by the deterministic Kairo demo provider.\n"})";
        }
        return {{}, {call}};
    }
};

fs::path DefaultSessionDirectory() {
    const char* home = std::getenv("HOME");
    fs::path base = home && *home ? fs::path(home) : fs::temp_directory_path();
#ifdef __HAIKU__
    return base / "config" / "settings" / "Kairo" / "sessions";
#else
    return base / ".local" / "share" / "kairo" / "sessions";
#endif
}

struct Options {
    fs::path project;
    fs::path sessions = DefaultSessionDirectory();
    std::string model = "qwen/qwen3.8-27b";
    std::string endpoint = "https://api.openai.com/v1";
    std::string key_environment = "KAIRO_API_KEY";
    std::string resume;
    bool fake = false;
    bool list = false;
};

void Usage() {
    std::cout << "Usage: kairo-cli --project PATH [--model NAME] [--endpoint URL]\n"
                 "                 [--key-env NAME] [--sessions PATH] [--resume ID]\n"
                 "                 [--fake] [--list-sessions]\n";
}

Options ParseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string argument = argv[i];
        auto value = [&]() -> std::string {
            if (++i >= argc) throw std::runtime_error("missing value after " + argument);
            return argv[i];
        };
        if (argument == "--project") options.project = value();
        else if (argument == "--model") options.model = value();
        else if (argument == "--endpoint") options.endpoint = value();
        else if (argument == "--key-env") options.key_environment = value();
        else if (argument == "--sessions") options.sessions = value();
        else if (argument == "--resume") options.resume = value();
        else if (argument == "--fake") options.fake = true;
        else if (argument == "--list-sessions") options.list = true;
        else if (argument == "--help" || argument == "-h") { Usage(); std::exit(0); }
        else throw std::runtime_error("unknown option: " + argument);
    }
    if (!options.list && options.project.empty()) throw std::runtime_error("--project is required");
    return options;
}

bool ApproveInTerminal(const kairo::ProposedAction& action) {
    std::cout << "\nApproval required\n" << TerminalSafe(action.preview)
              << "\nApprove this action? [y/N] " << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer)) return false;
    return answer == "y" || answer == "Y" || answer == "yes" || answer == "YES";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Options options = ParseOptions(argc, argv);
        kairo::SessionStore store(options.sessions);
        if (options.list) {
            for (const auto& id : store.List()) std::cout << id << '\n';
            return 0;
        }
        fs::path project = fs::canonical(options.project);
        kairo::Session session = options.resume.empty() ? store.Create(project, options.model) : store.Load(options.resume);
        if (fs::canonical(session.project) != project) throw std::runtime_error("resumed session belongs to another project");
        if (session.messages.empty()) {
            session.messages.push_back({kairo::Role::System,
                "You are Kairo, a local coding assistant running natively on Haiku. Inspect only the selected "
                "project using tools. Reads and searches are automatic; writes and shell commands require "
                "explicit approval. Use literal project-relative paths and '.' for the project root. For native "
                "Haiku GUI apps, use C++ with BApplication and BWindow, compile with g++, and link with -lbe. "
                "Do not probe for bcc, bchk, bimg, getbeospath, or /boot/develop, and do not use -nostdlib "
                "unless the project explicitly requires it. Treat command-not-found and linker diagnostics as "
                "failures even if a compound shell command reports exit code 0. Explain the final result "
                "concisely.", {}, {}});
        }

        std::shared_ptr<kairo::Provider> provider;
        if (options.fake) provider = std::make_shared<DemoProvider>();
        else provider = std::make_shared<kairo::OpenAIProvider>(kairo::OpenAIConfig{
            options.endpoint, options.key_environment, 120, {}});
        kairo::Limits limits;
        kairo::AgentEngine engine(provider,
            kairo::Workspace(project, limits.max_tool_output_bytes, {options.key_environment}),
            store, limits);
        kairo::CancellationToken cancellation;

        std::cout << "Kairo session " << session.id << " for " << project << "\n"
                  << "Enter a prompt, or /quit.\n";
        std::string prompt;
        while (std::cout << "> " << std::flush, std::getline(std::cin, prompt)) {
            if (prompt == "/quit" || prompt == "/exit") break;
            if (prompt.empty()) continue;
            bool streamed = false;
            auto events = [&](const kairo::EngineEvent& event) {
                const std::string safe = TerminalSafe(event.text);
                if (event.type == kairo::EventType::TextDelta) { std::cout << safe << std::flush; streamed = true; }
                else if (event.type == kairo::EventType::ToolStarted) std::cout << "\n[tool " << safe << "]\n";
                else if (event.type == kairo::EventType::ToolDenied) std::cout << "[denied] " << safe << '\n';
                else if (event.type == kairo::EventType::ToolFinished) std::cout << "[result] " << safe << '\n';
                else if (event.type == kairo::EventType::Error) std::cerr << "\n[error] " << safe << '\n';
            };
            std::string result = engine.Run(session, prompt, ApproveInTerminal, events, cancellation);
            if (!streamed && !result.empty()) std::cout << TerminalSafe(result);
            std::cout << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "kairo-cli: " << TerminalSafe(error.what()) << '\n';
        return 1;
    }
}
