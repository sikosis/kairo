#include "kairo/workspace.h"

#include "json.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

extern char** environ;

namespace kairo {
namespace {

bool IsWithin(const fs::path& root, const fs::path& candidate) {
    auto r = root.begin();
    auto c = candidate.begin();
    for (; r != root.end(); ++r, ++c) {
        if (c == candidate.end() || *r != *c) return false;
    }
    return true;
}

bool IsSensitiveEnvironmentName(const std::string& name,
                                const std::vector<std::string>& explicit_names) {
    if (std::find(explicit_names.begin(), explicit_names.end(), name) != explicit_names.end())
        return true;
    std::string upper;
    upper.reserve(name.size());
    for (unsigned char character : name)
        upper += static_cast<char>(std::toupper(character));
    constexpr const char* markers[] = {
        "API_KEY", "TOKEN", "SECRET", "PASSWORD", "PASSWD", "CREDENTIAL", "AUTH"
    };
    for (const char* marker : markers)
        if (upper.find(marker) != std::string::npos) return true;
    return upper == "SSH_AUTH_SOCK" || upper == "GIT_ASKPASS" ||
           upper == "SSH_ASKPASS";
}

std::vector<std::string> SanitizedEnvironment(const std::vector<std::string>& explicit_names) {
    std::vector<std::string> result;
    for (char** item = environ; item && *item; ++item) {
        std::string entry(*item);
        const std::size_t equals = entry.find('=');
        const std::string name = entry.substr(0, equals);
        if (!IsSensitiveEnvironmentName(name, explicit_names)) result.push_back(std::move(entry));
    }
    return result;
}

std::string ReadText(const fs::path& path, std::size_t limit) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open " + path.string());
    std::string contents;
    std::array<char, 4096> buffer{};
    while (input && contents.size() <= limit) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        std::streamsize count = input.gcount();
        if (count > 0) contents.append(buffer.data(), static_cast<std::size_t>(count));
    }
    const std::size_t inspected = std::min<std::size_t>(contents.size(), 4096);
    for (std::size_t index = 0; index < inspected; ++index) {
        unsigned char byte = static_cast<unsigned char>(contents[index]);
        if (byte == 0 || (byte < 0x20 && byte != '\n' && byte != '\r' && byte != '\t'))
            throw std::runtime_error("file appears to be binary and cannot be read as text: "
                                     + path.filename().string());
    }
    return contents;
}

std::string PreviewChange(const fs::path& path, const std::string& content) {
    std::ostringstream out;
    out << "Target: " << path << "\n";
    if (fs::exists(path)) {
        std::error_code error;
        const std::uintmax_t previous_size = fs::file_size(path, error);
        if (error) throw std::runtime_error("cannot inspect existing destination: " + path.string());
        out << "Replace " << previous_size << " bytes with " << content.size() << " bytes.\n";
    } else {
        out << "Create a new file containing " << content.size() << " bytes.\n";
    }
    std::string excerpt = content.substr(0, 1200);
    out << "New content preview:\n" << excerpt;
    if (excerpt.size() != content.size()) out << "\n...[preview truncated]";
    return out.str();
}

void WriteSecureTemporary(const fs::path& destination, const std::string& content) {
    std::string pattern = destination.string() + ".kairo.tmp.XXXXXX";
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    const int descriptor = ::mkstemp(writable.data());
    if (descriptor < 0) throw std::runtime_error("cannot create secure temporary file");
    const fs::path temporary(writable.data());
    bool ok = true;
    std::size_t offset = 0;
    while (offset < content.size()) {
        const ssize_t written = ::write(descriptor, content.data() + offset, content.size() - offset);
        if (written <= 0) { ok = false; break; }
        offset += static_cast<std::size_t>(written);
    }
    if (ok) ok = ::fsync(descriptor) == 0;
    if (::close(descriptor) != 0) ok = false;
    if (!ok) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw std::runtime_error("failed writing temporary file");
    }
    std::error_code error;
    fs::rename(temporary, destination, error);
    if (error) {
        fs::remove(temporary, error);
        throw std::runtime_error("cannot replace destination file");
    }
}

}  // namespace

Workspace::Workspace(fs::path root, std::size_t output_limit,
                     std::vector<std::string> sensitive_environment_variables)
    : root_(fs::canonical(std::move(root))), output_limit_(output_limit),
      sensitive_environment_variables_(std::move(sensitive_environment_variables)) {
    if (!fs::is_directory(root_)) throw std::runtime_error("project is not a directory: " + root_.string());
}

fs::path Workspace::ResolveInside(const std::string& relative, bool allow_missing) const {
    if (relative.empty())
        throw std::runtime_error("path is empty; use '.' for the project root");
    if (relative.find('$') != std::string::npos || relative.front() == '~')
        throw std::runtime_error("path must be literal and project-relative; environment variables "
                                 "and '~' are not expanded (use '.' for the project root)");

    fs::path requested(relative);
    fs::path combined = requested.is_absolute() ? requested : root_ / requested;
    std::error_code error;
    fs::path candidate = allow_missing
        ? fs::weakly_canonical(combined, error)
        : fs::canonical(combined, error);
    if (error)
        throw std::runtime_error("path does not exist in the project: " + relative);
    if (!IsWithin(root_, candidate)) throw std::runtime_error("path escapes project: " + relative);
    return candidate;
}

std::string Workspace::Limit(std::string value) const {
    if (value.size() <= output_limit_) return value;
    value.resize(output_limit_);
    value += "\n...[tool output truncated]";
    return value;
}

ToolResult Workspace::ReadFile(const std::string& arguments) const {
    const auto root = json::Parse(arguments);
    fs::path path = ResolveInside(root.GetString("path"), false);
    if (!fs::is_regular_file(path)) throw std::runtime_error("not a regular file: " + path.string());
    return {true, false, Limit(ReadText(path, output_limit_))};
}

ToolResult Workspace::SearchFiles(const std::string& arguments) const {
    const auto root = json::Parse(arguments);
    const std::string query = root.GetString("query");
    if (query.empty()) throw std::runtime_error("search query is empty");
    fs::path start = ResolveInside(root.GetString("path", "."), false);
    std::ostringstream matches;
    std::error_code error;
    fs::recursive_directory_iterator iterator(start, fs::directory_options::skip_permission_denied, error), end;
    for (; iterator != end && !error; iterator.increment(error)) {
        if (!iterator->is_regular_file(error)) continue;
        fs::path canonical = fs::canonical(iterator->path(), error);
        if (error || !IsWithin(root_, canonical)) { error.clear(); continue; }
        if (iterator->file_size(error) > 2 * 1024 * 1024) continue;
        std::ifstream input(canonical);
        std::string line;
        std::size_t number = 0;
        while (std::getline(input, line)) {
            ++number;
            if (line.find(query) != std::string::npos)
                matches << fs::relative(canonical, root_).string() << ':' << number << ':' << line << '\n';
            if (matches.tellp() >= static_cast<std::streampos>(output_limit_)) return {true, false, Limit(matches.str())};
        }
    }
    return {true, false, matches.str().empty() ? "No matches." : matches.str()};
}

ToolResult Workspace::WriteFile(const ToolCall& call, const ApprovalHandler& approval) const {
    const auto root = json::Parse(call.arguments);
    const std::string relative = root.GetString("path");
    const std::string content = root.GetString("content");
    fs::path path = ResolveInside(relative, true);
    ProposedAction action{ActionKind::WriteFile, call.id, path.string(), root_.string(), PreviewChange(path, content)};
    if (!approval(action)) return {false, true, "Write denied by user; no file was changed."};
    fs::create_directories(path.parent_path());
    path = ResolveInside(relative, true);
    WriteSecureTemporary(path, content);
    return {true, false, "Wrote " + std::to_string(content.size()) + " bytes to " + relative};
}

ToolResult Workspace::RunShell(const ToolCall& call, const ApprovalHandler& approval,
                               const CancellationToken& cancellation) const {
    const auto root = json::Parse(call.arguments);
    const std::string command = root.GetString("command");
    fs::path cwd = ResolveInside(root.GetString("working_directory", "."), false);
    if (!fs::is_directory(cwd)) throw std::runtime_error("working directory is not a directory");
    ProposedAction action{ActionKind::ShellCommand, call.id, command, cwd.string(),
                          "Command: " + command + "\nWorking directory: " + cwd.string()};
    if (!approval(action)) return {false, true, "Shell command denied by user; it was not executed."};

    std::vector<std::string> environment = SanitizedEnvironment(sensitive_environment_variables_);
    std::vector<char*> environment_pointers;
    environment_pointers.reserve(environment.size() + 1);
    for (std::string& entry : environment) environment_pointers.push_back(entry.data());
    environment_pointers.push_back(nullptr);

    int output_pipe[2];
    if (::pipe(output_pipe) != 0) throw std::runtime_error("pipe failed");
    pid_t child = ::fork();
    if (child < 0) { ::close(output_pipe[0]); ::close(output_pipe[1]); throw std::runtime_error("fork failed"); }
    if (child == 0) {
        ::setpgid(0, 0);
        ::close(output_pipe[0]);
        ::dup2(output_pipe[1], STDOUT_FILENO);
        ::dup2(output_pipe[1], STDERR_FILENO);
        ::close(output_pipe[1]);
        const int null_input = ::open("/dev/null", O_RDONLY);
        if (null_input >= 0) {
            ::dup2(null_input, STDIN_FILENO);
            ::close(null_input);
        }
        if (::chdir(cwd.c_str()) != 0) _exit(126);
        char* arguments[] = {const_cast<char*>("sh"), const_cast<char*>("-c"),
                             const_cast<char*>(command.c_str()), nullptr};
        ::execve("/bin/sh", arguments, environment_pointers.data());
        _exit(127);
    }
    ::setpgid(child, child);
    ::close(output_pipe[1]);
    ::fcntl(output_pipe[0], F_SETFL, ::fcntl(output_pipe[0], F_GETFL) | O_NONBLOCK);
    std::string output;
    int status = 0;
    for (;;) {
        std::array<char, 4096> buffer{};
        ssize_t count;
        while ((count = ::read(output_pipe[0], buffer.data(), buffer.size())) > 0) {
            if (output.size() < output_limit_) output.append(buffer.data(), std::min<std::size_t>(count, output_limit_ - output.size()));
        }
        pid_t result = ::waitpid(child, &status, WNOHANG);
        if (result == child) break;
        if (cancellation.IsCancelled()) {
            ::kill(-child, SIGTERM);
            ::waitpid(child, &status, 0);
            ::close(output_pipe[0]);
            return {false, false, Limit(output + "\nCommand cancelled.")};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::array<char, 4096> buffer{};
    ssize_t count;
    while ((count = ::read(output_pipe[0], buffer.data(), buffer.size())) > 0) {
        if (output.size() < output_limit_) output.append(buffer.data(), std::min<std::size_t>(count, output_limit_ - output.size()));
    }
    ::close(output_pipe[0]);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    output += "\nExit code: " + std::to_string(exit_code);
    return {exit_code == 0, false, Limit(output)};
}

ToolResult Workspace::Execute(const ToolCall& call, const ApprovalHandler& approval,
                              const CancellationToken& cancellation) const {
    try {
        if (call.name == "read_file") return ReadFile(call.arguments);
        if (call.name == "search_files") return SearchFiles(call.arguments);
        if (call.name == "write_file") return WriteFile(call, approval);
        if (call.name == "run_shell") return RunShell(call, approval, cancellation);
        return {false, false, "Unknown tool: " + call.name};
    } catch (const std::exception& error) {
        return {false, false, std::string("Tool error: ") + error.what()};
    }
}

}  // namespace kairo
