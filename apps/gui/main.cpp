#ifndef __HAIKU__
#error "The native Kairo GUI must be compiled on Haiku."
#endif

#include "kairo/agent_engine.h"
#include "kairo/openai_provider.h"

#include <Alert.h>
#include <Application.h>
#include <Box.h>
#include <Button.h>
#include <CheckBox.h>
#include <Entry.h>
#include <File.h>
#include <FilePanel.h>
#include <FindDirectory.h>
#include <Font.h>
#include <Invoker.h>
#include <LayoutBuilder.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <Message.h>
#include <Messenger.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <ScrollView.h>
#include <Size.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#include <Window.h>

#include <algorithm>
#include <condition_variable>
#include <chrono>
#include <ctime>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

constexpr uint32 kSend = 'send';
constexpr uint32 kCancel = 'cncl';
constexpr uint32 kChooseProject = 'chpr';
constexpr uint32 kNewSession = 'news';
constexpr uint32 kClearTranscript = 'cltr';
constexpr uint32 kFocusPrompt = 'fcpr';
constexpr uint32 kQuit = 'quit';
constexpr uint32 kOpenSettings = 'opst';
constexpr uint32 kSaveSettings = 'svst';
constexpr uint32 kActivateSettings = 'stac';
constexpr uint32 kApprovalModeChanged = 'apmd';
constexpr uint32 kEngineEvent = 'kevt';
constexpr uint32 kApprovalRequest = 'kapr';
constexpr uint32 kApprovalDecision = 'kapd';

constexpr const char* kModels[] = {
    "qwen/qwen3.8-27b",
    "openai/gpt-oss-120b",
    "openai/gpt-oss-20b",
    "gpt-4.1-mini",
    "gpt-4.1",
    "gpt-4.1-nano",
    "o4-mini",
    "o3",
};

const char* ReadyStatus(const char* configured_key = nullptr) {
    const char* key = configured_key && *configured_key
        ? configured_key : std::getenv("KAIRO_API_KEY");
    return key && *key ? "Ready - API key configured" : "Ready - API key missing";
}

struct PendingApproval {
    std::mutex mutex;
    std::condition_variable changed;
    bool resolved = false;
    bool approved = false;

    bool Wait(const kairo::CancellationToken& cancellation) {
        std::unique_lock<std::mutex> lock(mutex);
        while (!resolved && !cancellation.IsCancelled())
            changed.wait_for(lock, std::chrono::milliseconds(100));
        return resolved && approved;
    }
    void Resolve(bool value) {
        std::lock_guard<std::mutex> lock(mutex);
        if (resolved) return;
        resolved = true;
        approved = value;
        changed.notify_all();
    }
};

fs::path SessionDirectory() {
    BPath path;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
        return fs::temp_directory_path() / "Kairo" / "sessions";
    return fs::path(path.Path()) / "Kairo" / "sessions";
}

fs::path LogPath() {
    return SessionDirectory().parent_path() / "kairo.log";
}

void WriteLog(const char* level, const std::string& message) noexcept {
    static std::mutex log_mutex;
    std::lock_guard<std::mutex> lock(log_mutex);
    try {
        fs::path path = LogPath();
        std::error_code error;
        fs::create_directories(path.parent_path(), error);
        if (error || chmod(path.parent_path().c_str(), S_IRWXU) != 0) return;

        int descriptor = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND,
                              S_IRUSR | S_IWUSR);
        if (descriptor < 0) return;
        if (fchmod(descriptor, S_IRUSR | S_IWUSR) != 0) {
            close(descriptor);
            return;
        }

        std::time_t now = std::time(nullptr);
        std::tm local_time{};
        char timestamp[32] = "unknown-time";
        if (localtime_r(&now, &local_time))
            std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local_time);
        std::string line = std::string(timestamp) + " [" + level + "] " + message + "\n";
        const char* data = line.data();
        size_t remaining = line.size();
        while (remaining > 0) {
            ssize_t written = write(descriptor, data, remaining);
            if (written <= 0) break;
            data += written;
            remaining -= static_cast<size_t>(written);
        }
        close(descriptor);
    } catch (...) {
        // Logging must never take down the GUI.
    }
}

struct ProviderSettings {
    std::string endpoint = "https://api.openai.com/v1";
    std::string api_key;
};

enum class TranscriptStyle {
    Assistant,
    User,
    Tool,
    ToolOutput,
    Warning,
    Error,
};

fs::path ProviderSettingsPath() {
    return SessionDirectory().parent_path() / "provider_settings";
}

ProviderSettings LoadProviderSettings() {
    ProviderSettings settings;
    if (const char* endpoint = std::getenv("KAIRO_BASE_URL"); endpoint && *endpoint)
        settings.endpoint = endpoint;
    if (const char* key = std::getenv("KAIRO_API_KEY"); key && *key)
        settings.api_key = key;

    BFile file(ProviderSettingsPath().c_str(), B_READ_ONLY);
    BMessage archive;
    if (file.InitCheck() != B_OK || archive.Unflatten(&file) != B_OK) return settings;
    const char* value = nullptr;
    if (archive.FindString("endpoint", &value) == B_OK && value && *value)
        settings.endpoint = value;
    if (archive.FindString("api_key", &value) == B_OK && value)
        settings.api_key = value;
    return settings;
}

status_t SaveProviderSettings(const ProviderSettings& settings) {
    fs::path path = ProviderSettingsPath();
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) return B_ERROR;
    if (chmod(path.parent_path().c_str(), S_IRWXU) != 0) return B_ERROR;

    std::string pattern = path.string() + ".tmp.XXXXXX";
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    int descriptor = mkstemp(writable.data());
    if (descriptor < 0) return B_ERROR;
    fs::path temporary(writable.data());
    close(descriptor);
    if (chmod(temporary.c_str(), S_IRUSR | S_IWUSR) != 0) {
        fs::remove(temporary, error);
        return B_ERROR;
    }

    BMessage archive('kpst');
    archive.AddString("endpoint", settings.endpoint.c_str());
    archive.AddString("api_key", settings.api_key.c_str());
    status_t result;
    {
        BFile file(temporary.c_str(), B_WRITE_ONLY | B_ERASE_FILE);
        result = file.InitCheck();
        if (result == B_OK) result = archive.Flatten(&file);
    }
    if (result != B_OK) {
        fs::remove(temporary, error);
        return result;
    }
    fs::rename(temporary, path, error);
    if (error) {
        fs::remove(temporary, error);
        return B_ERROR;
    }
    return B_OK;
}

class ProviderSettingsWindow : public BWindow {
public:
    ProviderSettingsWindow(BMessenger target, const ProviderSettings& settings)
        : BWindow(BRect(180, 180, 800, 370), "Kairo Provider Settings",
                  B_TITLED_WINDOW, B_NOT_ZOOMABLE | B_NOT_RESIZABLE
                      | B_AUTO_UPDATE_SIZE_LIMITS),
          target_(target) {
        endpoint_ = new BTextControl("settings-endpoint", "API URL",
                                     settings.endpoint.c_str(), nullptr);
        api_key_ = new BTextControl("settings-api-key", "API key",
                                    settings.api_key.c_str(), nullptr);
        api_key_->TextView()->HideTyping(true);
        endpoint_->SetExplicitMinSize(BSize(520, B_SIZE_UNSET));
        api_key_->SetExplicitMinSize(BSize(520, B_SIZE_UNSET));
        auto* cancel = new BButton("settings-cancel", "Cancel",
                                   new BMessage(B_QUIT_REQUESTED));
        auto* save = new BButton("settings-save", "Save",
                                 new BMessage(kSaveSettings));
        SetDefaultButton(save);

        BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
            .SetInsets(B_USE_WINDOW_INSETS)
            .Add(new BStringView("provider-name", "Provider: OpenAI-compatible"))
            .Add(endpoint_)
            .Add(api_key_)
            .Add(new BStringView("credential-note",
                "The API key is stored locally with owner-only permissions."))
            .AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
                .AddGlue()
                .Add(cancel)
                .Add(save)
            .End();
    }

    void MessageReceived(BMessage* message) override {
        if (message->what == kActivateSettings) {
            Activate(true);
            return;
        }
        if (message->what != kSaveSettings) {
            BWindow::MessageReceived(message);
            return;
        }
        if (std::strlen(endpoint_->Text()) == 0) {
            endpoint_->MakeFocus(true);
            return;
        }
        BMessage saved(kSaveSettings);
        saved.AddString("endpoint", endpoint_->Text());
        saved.AddString("api_key", api_key_->Text());
        target_.SendMessage(&saved);
        PostMessage(B_QUIT_REQUESTED);
    }

private:
    BMessenger target_;
    BTextControl* endpoint_;
    BTextControl* api_key_;
};

class KairoWindow : public BWindow {
public:
    KairoWindow()
        : BWindow(BRect(80, 80, 1180, 830), "Kairo", B_TITLED_WINDOW,
                  B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS) {
        menu_bar_ = new BMenuBar("menu-bar");
        auto* project_menu = new BMenu("Project");
        choose_project_item_ = new BMenuItem("Choose Project...", new BMessage(kChooseProject), 'O');
        project_menu->AddItem(choose_project_item_);
        project_menu->AddSeparatorItem();
        project_menu->AddItem(new BMenuItem("Quit Kairo", new BMessage(kQuit), 'Q'));
        menu_bar_->AddItem(project_menu);

        auto* session_menu = new BMenu("Session");
        new_session_item_ = new BMenuItem("New Session", new BMessage(kNewSession), 'N');
        session_menu->AddItem(new_session_item_);
        session_menu->AddItem(new BMenuItem("Clear Conversation", new BMessage(kClearTranscript), 'L'));
        menu_bar_->AddItem(session_menu);

        auto* run_menu = new BMenu("Run");
        focus_prompt_item_ = new BMenuItem("Focus Prompt", new BMessage(kFocusPrompt), 'P');
        send_item_ = new BMenuItem("Send Prompt", new BMessage(kSend), 'R');
        cancel_item_ = new BMenuItem("Cancel Run", new BMessage(kCancel), 'K');
        cancel_item_->SetEnabled(false);
        run_menu->AddItem(focus_prompt_item_);
        run_menu->AddSeparatorItem();
        run_menu->AddItem(send_item_);
        run_menu->AddItem(cancel_item_);
        menu_bar_->AddItem(run_menu);

        auto* settings_menu = new BMenu("Settings");
        settings_item_ = new BMenuItem("Provider Settings...",
                                       new BMessage(kOpenSettings), ',');
        settings_menu->AddItem(settings_item_);
        menu_bar_->AddItem(settings_menu);

        auto* help_menu = new BMenu("Help");
        help_menu->AddItem(new BMenuItem("About Kairo...", new BMessage(B_ABOUT_REQUESTED)));
        menu_bar_->AddItem(help_menu);

        project_ = new BTextControl("project", "Project", "/boot/home/work", nullptr);
        choose_ = new BButton("choose", "Choose", new BMessage(kChooseProject));
        session_ = new BTextControl("session", "Resume session", "", nullptr);
        provider_settings_ = LoadProviderSettings();
        settings_ = new BButton("settings", "Settings...", new BMessage(kOpenSettings));
        model_menu_ = new BPopUpMenu("model-options", true, true);
        for (const char* model : kModels)
            model_menu_->AddItem(new BMenuItem(model, nullptr));
        model_menu_->ItemAt(0)->SetMarked(true);
        model_ = new BMenuField("model", "Model", model_menu_);
        approval_checkbox_ = new BCheckBox("approvals",
            "Ask before file writes and shell commands",
            new BMessage(kApprovalModeChanged));
        approval_checkbox_->SetValue(B_CONTROL_ON);
        transcript_ = new BTextView("transcript");
        transcript_->SetStylable(true);
        transcript_->MakeEditable(false);
        prompt_ = new BTextView("prompt");
        status_ = new BStringView("status", ReadyStatus(provider_settings_.api_key.c_str()));
        send_ = new BButton("send", "Send", new BMessage(kSend));
        cancel_ = new BButton("cancel", "Cancel", new BMessage(kCancel));
        cancel_->SetEnabled(false);

        auto* transcript_scroll = new BScrollView("transcript-scroll", transcript_, 0, false, true);
        auto* prompt_scroll = new BScrollView("prompt-scroll", prompt_, 0, false, true);
        project_->SetExplicitMinSize(BSize(650, B_SIZE_UNSET));
        session_->SetExplicitMinSize(BSize(500, B_SIZE_UNSET));
        model_->SetExplicitMinSize(BSize(300, B_SIZE_UNSET));
        transcript_scroll->SetExplicitMinSize(BSize(800, 320));
        prompt_scroll->SetExplicitMinSize(BSize(800, 140));
        auto* setup_box = new BBox("setup-box");
        setup_box->SetLabel("Project and session");
        BLayoutBuilder::Group<>(setup_box, B_VERTICAL, B_USE_DEFAULT_SPACING)
            .SetInsets(B_USE_DEFAULT_SPACING)
            .AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
                .Add(project_, 1.0f)
                .Add(choose_)
            .End()
            .AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
                .Add(session_, 1.0f)
                .Add(model_)
                .Add(settings_)
            .End()
            .AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
                .Add(approval_checkbox_)
                .AddGlue()
            .End();

        BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
            .SetInsets(0)
            .Add(menu_bar_)
            .AddGroup(B_VERTICAL, B_USE_DEFAULT_SPACING)
                .SetInsets(B_USE_WINDOW_INSETS)
                .Add(setup_box)
                .Add(new BStringView("conversation-label", "Conversation"))
                .Add(transcript_scroll, 1.0f)
                .Add(new BStringView("prompt-label", "Prompt"))
                .Add(prompt_scroll, 0.28f)
                .AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
                    .Add(status_, 1.0f)
                    .Add(cancel_)
                    .Add(send_)
                .End()
            .End();
        WriteLog("INFO", "Kairo GUI started");
    }

    ~KairoWindow() override {
        WriteLog("INFO", "Kairo GUI stopping");
        cancellation_.Cancel();
        ResolveAllApprovals(false);
        if (worker_.joinable()) worker_.join();
        delete project_panel_;
    }

    bool QuitRequested() override {
        cancellation_.Cancel();
        ResolveAllApprovals(false);
        be_app->PostMessage(B_QUIT_REQUESTED);
        return true;
    }

    void MessageReceived(BMessage* message) override {
        switch (message->what) {
            case kSend: StartRun(); break;
            case kCancel:
                WriteLog("INFO", "Run cancellation requested");
                cancellation_.Cancel();
                ResolveAllApprovals(false);
                status_->SetText("Cancelling");
                break;
            case kChooseProject: ChooseProject(); break;
            case kNewSession: NewSession(); break;
            case kClearTranscript: transcript_->SetText(""); break;
            case kFocusPrompt: prompt_->MakeFocus(true); break;
            case kQuit: be_app->PostMessage(B_QUIT_REQUESTED); break;
            case kOpenSettings: OpenSettings(); break;
            case kSaveSettings: ReceiveSettings(message); break;
            case kApprovalModeChanged: ConfirmApprovalMode(); break;
            case B_ABOUT_REQUESTED: ShowAbout(); break;
            case B_REFS_RECEIVED: ReceiveProject(message); break;
            case kEngineEvent: ReceiveEngineEvent(message); break;
            case kApprovalRequest: ShowApproval(message); break;
            case kApprovalDecision: FinishApproval(message); break;
            default: BWindow::MessageReceived(message);
        }
    }

private:
    void AppendTranscript(const std::string& text, TranscriptStyle style,
                          bool bold = false) {
        if (text.empty()) return;
        rgb_color color{35, 35, 35, 255};
        BFont font;
        font = *be_plain_font;
        switch (style) {
            case TranscriptStyle::Assistant:
                color = {35, 35, 35, 255};
                break;
            case TranscriptStyle::User:
                color = {25, 90, 175, 255};
                break;
            case TranscriptStyle::Tool:
                color = {115, 65, 155, 255};
                break;
            case TranscriptStyle::ToolOutput:
                color = {55, 90, 105, 255};
                font = *be_fixed_font;
                break;
            case TranscriptStyle::Warning:
                color = {185, 105, 0, 255};
                break;
            case TranscriptStyle::Error:
                color = {190, 35, 45, 255};
                break;
        }
        if (bold) font.SetFace(B_BOLD_FACE);
        int32 start = transcript_->TextLength();
        transcript_->Insert(start, text.c_str(), text.size());
        rgb_color applied_color = color;
        transcript_->SetFontAndColor(start, start + static_cast<int32>(text.size()),
                                     &font, B_FONT_ALL, &applied_color);
        transcript_->ScrollToOffset(transcript_->TextLength());
    }

    void NewSession() {
        if (worker_.joinable()) return;
        session_->SetText("");
        transcript_->SetText("");
        status_->SetText(ReadyStatus(provider_settings_.api_key.c_str()));
        prompt_->MakeFocus(true);
    }

    void OpenSettings() {
        if (settings_messenger_.IsValid()) {
            if (settings_messenger_.SendMessage(kActivateSettings) == B_OK) return;
            settings_messenger_ = BMessenger();
        }
        WriteLog("INFO", "Provider settings opened");
        auto* settings_window = new ProviderSettingsWindow(BMessenger(this), provider_settings_);
        settings_messenger_ = BMessenger(settings_window);
        settings_window->MoveTo(Frame().left + 100, Frame().top + 80);
        settings_window->Show();
    }

    void ReceiveSettings(BMessage* message) {
        const char* endpoint = nullptr;
        const char* api_key = nullptr;
        if (message->FindString("endpoint", &endpoint) != B_OK || !endpoint || !*endpoint)
            return;
        message->FindString("api_key", &api_key);
        ProviderSettings updated{endpoint, api_key ? api_key : ""};
        try {
            (void)kairo::OpenAIProvider(kairo::OpenAIConfig{
                updated.endpoint, "KAIRO_API_KEY", 120, {}});
        } catch (const std::exception& error) {
            WriteLog("WARNING", "Invalid provider URL rejected");
            (new BAlert("settings-error", error.what(), "OK", nullptr, nullptr,
                        B_WIDTH_AS_USUAL, B_STOP_ALERT))->Go();
            return;
        }
        if (SaveProviderSettings(updated) != B_OK) {
            WriteLog("ERROR", "Provider settings could not be saved");
            (new BAlert("settings-error", "Kairo could not save the provider settings.",
                        "OK", nullptr, nullptr, B_WIDTH_AS_USUAL, B_STOP_ALERT))->Go();
            return;
        }
        provider_settings_ = std::move(updated);
        WriteLog("INFO", "Provider settings saved");
        status_->SetText(ReadyStatus(provider_settings_.api_key.c_str()));
    }

    void ConfirmApprovalMode() {
        if (approval_checkbox_->Value() == B_CONTROL_ON) {
            WriteLog("INFO", "Per-action approvals enabled");
            return;
        }
        BAlert* warning = new BAlert("unsafe-approvals",
            "Disabling approvals lets model-generated file writes and shell commands run "
            "without review. Those commands have your user account's filesystem access.",
            "Keep enabled", "Disable approvals", nullptr,
            B_WIDTH_AS_USUAL, B_WARNING_ALERT);
        warning->SetShortcut(0, B_ESCAPE);
        if (warning->Go() != 1) {
            approval_checkbox_->SetValue(B_CONTROL_ON);
            WriteLog("INFO", "Per-action approvals kept enabled");
            return;
        }
        WriteLog("WARNING", "Per-action approvals disabled for the next run");
    }

    void ShowAbout() {
        BAlert* alert = new BAlert("about-kairo",
            "Kairo\n\nA native Haiku coding assistant using the Application Kit, "
            "Interface Kit, and a shared C++ agent engine.", "OK", nullptr, nullptr,
            B_WIDTH_AS_USUAL, B_INFO_ALERT);
        alert->Go();
    }

    void ChooseProject() {
        if (!project_panel_) {
            BMessenger* target = new BMessenger(this);
            project_panel_ = new BFilePanel(B_OPEN_PANEL, target, nullptr, B_DIRECTORY_NODE, false);
            project_panel_->SetButtonLabel(B_DEFAULT_BUTTON, "Choose");
        }
        project_panel_->Show();
    }

    void ReceiveProject(BMessage* message) {
        entry_ref reference;
        if (message->FindRef("refs", &reference) != B_OK) return;
        BEntry entry(&reference, true);
        BPath path;
        if (entry.GetPath(&path) == B_OK) project_->SetText(path.Path());
    }

    void StartRun() {
        if (worker_.joinable()) return;
        std::string project = project_->Text();
        std::string prompt(prompt_->Text(), prompt_->TextLength());
        if (project.empty() || prompt.empty()) { status_->SetText("Choose a project and enter a prompt"); return; }
        std::string api_key = provider_settings_.api_key;
        const char* environment_key = std::getenv("KAIRO_API_KEY");
        if (api_key.empty() && (!environment_key || !*environment_key)) {
            WriteLog("WARNING", "Run blocked because no API key is configured");
            status_->SetText("Configure an API key in Settings");
            OpenSettings();
            return;
        }
        std::string endpoint = provider_settings_.endpoint;
        if (endpoint.empty()) endpoint = "https://api.openai.com/v1";
        prompt_->SetText("");
        AppendTranscript("\nYou: ", TranscriptStyle::User, true);
        AppendTranscript(prompt, TranscriptStyle::User);
        AppendTranscript("\n\nKairo: ", TranscriptStyle::Assistant, true);
        SetRunning(true);
        cancellation_ = kairo::CancellationToken{};
        BMessenger target(this);
        std::string session_id = session_->Text();
        BMenuItem* selected_model = model_menu_->FindMarked();
        std::string model = selected_model ? selected_model->Label() : kModels[0];
        bool require_approvals = approval_checkbox_->Value() == B_CONTROL_ON;
        WriteLog("INFO", std::string("Run started with model ") + model);
        worker_ = std::thread([this, target, project, prompt, session_id, model,
                               endpoint, api_key, require_approvals]() mutable {
            bool engine_started = false;
            try {
                kairo::SessionStore store(SessionDirectory());
                kairo::Session session = session_id.empty() ? store.Create(project, model) : store.Load(session_id);
                if (fs::canonical(session.project) != fs::canonical(project))
                    throw std::runtime_error("The resumed session belongs to another project");
                if (session.model != model) {
                    WriteLog("INFO", std::string("Resumed session model changed from ")
                        + session.model + " to " + model);
                    session.model = model;
                }
                if (session.messages.empty()) session.messages.push_back({kairo::Role::System,
                    "You are Kairo, a local coding assistant running natively on Haiku. Reads and searches "
                    "stay inside the selected project. Use literal project-relative paths and '.' for the "
                    "project root; do not inspect $HOME or other system locations. For native Haiku GUI apps, "
                    "write C++ using BApplication and BWindow, compile with g++, and link with -lbe. Do not "
                    "probe for bcc, bchk, bimg, getbeospath, or /boot/develop, and do not use -nostdlib unless "
                    "the project explicitly requires it. Treat command-not-found and linker diagnostics as "
                    "failures even if a compound shell command reports exit code 0. Writes and shell commands "
                    "require approval.", {}, {}});
                auto provider = std::make_shared<kairo::OpenAIProvider>(kairo::OpenAIConfig{
                    endpoint, "KAIRO_API_KEY", 120, api_key});
                kairo::Limits limits;
                kairo::AgentEngine engine(provider,
                    kairo::Workspace(project, limits.max_tool_output_bytes, {"KAIRO_API_KEY"}),
                    store, limits);
                auto approval = [this, target, require_approvals](const kairo::ProposedAction& action) {
                    if (!require_approvals) return true;
                    auto pending = std::make_shared<PendingApproval>();
                    {
                        std::lock_guard<std::mutex> lock(approvals_mutex_);
                        approvals_.push_back(pending);
                    }
                    BMessage request(kApprovalRequest);
                    request.AddPointer("approval", pending.get());
                    request.AddString("preview", action.preview.c_str());
                    if (target.SendMessage(&request) != B_OK) { pending->Resolve(false); return false; }
                    return pending->Wait(cancellation_);
                };
                auto events = [target](const kairo::EngineEvent& event) {
                    BMessage update(kEngineEvent);
                    update.AddInt32("type", static_cast<int32>(event.type));
                    update.AddString("text", event.text.c_str());
                    update.AddString("session", event.session_id.c_str());
                    target.SendMessage(&update);
                };
                engine_started = true;
                engine.Run(session, prompt, approval, events, cancellation_);
            } catch (const std::exception& error) {
                WriteLog("ERROR", "Run failed; details were shown in the GUI");
                if (!engine_started) {
                    BMessage update(kEngineEvent);
                    update.AddInt32("type", static_cast<int32>(kairo::EventType::Error));
                    update.AddString("text", error.what());
                    update.AddString("session", session_id.c_str());
                    target.SendMessage(&update);
                }
            }
            BMessage done(kEngineEvent);
            done.AddInt32("type", static_cast<int32>(kairo::EventType::Completed));
            done.AddBool("worker_done", true);
            target.SendMessage(&done);
        });
    }

    void ReceiveEngineEvent(BMessage* message) {
        bool worker_done = false;
        message->FindBool("worker_done", &worker_done);
        if (worker_done) {
            ResolveAllApprovals(false);
            if (worker_.joinable()) worker_.join();
            WriteLog("INFO", "Run worker stopped");
            SetRunning(false);
            return;
        }
        int32 raw_type = 0;
        const char* text = nullptr;
        const char* session = nullptr;
        message->FindInt32("type", &raw_type);
        if (message->FindString("text", &text) != B_OK || !text) text = "";
        if (message->FindString("session", &session) != B_OK || !session) session = "";
        auto type = static_cast<kairo::EventType>(raw_type);
        if (type == kairo::EventType::SessionSaved && *session
            && std::strlen(session_->Text()) == 0)
            session_->SetText(session);
        if (type == kairo::EventType::TextDelta) {
            AppendTranscript(text, TranscriptStyle::Assistant);
        } else if (type == kairo::EventType::ToolStarted) {
            status_->SetText((std::string("Running ") + text).c_str());
            AppendTranscript(std::string("\n\n[tool: ") + text + "]\n",
                             TranscriptStyle::Tool, true);
        }
        else if (type == kairo::EventType::ToolFinished || type == kairo::EventType::ToolDenied) {
            const bool denied = type == kairo::EventType::ToolDenied;
            AppendTranscript(denied ? "[denied]\n" : "[tool result]\n",
                             denied ? TranscriptStyle::Warning : TranscriptStyle::Tool, true);
            AppendTranscript(text, denied ? TranscriptStyle::Warning
                                          : TranscriptStyle::ToolOutput);
            AppendTranscript("\n\nKairo: ", TranscriptStyle::Assistant, true);
        }
        else if (type == kairo::EventType::Completed) {
            WriteLog("INFO", "Run completed");
            status_->SetText("Complete");
        }
        else if (type == kairo::EventType::Cancelled) {
            WriteLog("INFO", "Run cancelled");
            status_->SetText("Cancelled");
        }
        else if (type == kairo::EventType::Error) {
            status_->SetText("Error");
            if (std::strncmp(text, "session not found:", 18) == 0)
                session_->SetText("");
            AppendTranscript("\nError: ", TranscriptStyle::Error, true);
            AppendTranscript(text, TranscriptStyle::Error);
        }
    }

    void ShowApproval(BMessage* message) {
        void* pointer = nullptr;
        const char* preview = nullptr;
        if (message->FindPointer("approval", &pointer) != B_OK || !pointer) return;
        if (message->FindString("preview", &preview) != B_OK || !preview)
            preview = "No action preview was supplied.";
        BMessage* decision = new BMessage(kApprovalDecision);
        decision->AddPointer("approval", pointer);
        BAlert* alert = new BAlert("approval", preview, "Deny", "Approve", nullptr,
                                   B_WIDTH_AS_USUAL, B_WARNING_ALERT);
        alert->SetShortcut(0, B_ESCAPE);
        alert->Go(new BInvoker(decision, this));
    }

    void FinishApproval(BMessage* message) {
        void* pointer = nullptr;
        int32 which = 0;
        if (message->FindPointer("approval", &pointer) != B_OK || !pointer) return;
        message->FindInt32("which", &which);
        std::shared_ptr<PendingApproval> pending;
        {
            std::lock_guard<std::mutex> lock(approvals_mutex_);
            auto found = std::find_if(approvals_.begin(), approvals_.end(),
                [pointer](const std::shared_ptr<PendingApproval>& candidate) {
                    return candidate.get() == pointer;
                });
            if (found == approvals_.end()) return;
            pending = *found;
            approvals_.erase(found);
        }
        pending->Resolve(which == 1);
    }

    void ResolveAllApprovals(bool approved) {
        std::lock_guard<std::mutex> lock(approvals_mutex_);
        for (const auto& pending : approvals_) pending->Resolve(approved);
        approvals_.clear();
    }

    void SetRunning(bool running) {
        send_->SetEnabled(!running);
        cancel_->SetEnabled(running);
        project_->SetEnabled(!running);
        choose_->SetEnabled(!running);
        session_->SetEnabled(!running);
        model_->SetEnabled(!running);
        approval_checkbox_->SetEnabled(!running);
        settings_->SetEnabled(!running);
        choose_project_item_->SetEnabled(!running);
        new_session_item_->SetEnabled(!running);
        settings_item_->SetEnabled(!running);
        send_item_->SetEnabled(!running);
        cancel_item_->SetEnabled(running);
        if (running) status_->SetText("Working");
    }

    BMenuBar* menu_bar_;
    BMenuItem* choose_project_item_;
    BMenuItem* new_session_item_;
    BMenuItem* focus_prompt_item_;
    BMenuItem* send_item_;
    BMenuItem* cancel_item_;
    BMenuItem* settings_item_;
    BTextControl* project_;
    BButton* choose_;
    BTextControl* session_;
    BButton* settings_;
    BMenuField* model_;
    BPopUpMenu* model_menu_;
    BCheckBox* approval_checkbox_;
    BTextView* transcript_;
    BTextView* prompt_;
    BStringView* status_;
    BButton* send_;
    BButton* cancel_;
    std::thread worker_;
    kairo::CancellationToken cancellation_;
    std::mutex approvals_mutex_;
    std::vector<std::shared_ptr<PendingApproval>> approvals_;
    BFilePanel* project_panel_ = nullptr;
    ProviderSettings provider_settings_;
    BMessenger settings_messenger_;
};

class KairoApplication : public BApplication {
public:
    KairoApplication() : BApplication("application/x-vnd.Kairo-Agent") {}
    void ReadyToRun() override {
        auto* window = new KairoWindow();
        window->Show();
    }
};

}  // namespace

int main() {
    KairoApplication application;
    application.Run();
    return 0;
}
