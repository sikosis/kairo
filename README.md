# Kairo for Haiku

Kairo is a native Haiku coding assistant with a shared portable C++ engine. The Haiku `BApplication` and the command-line client call the same engine, provider interface, workspace policy, approval flow, and session store. The first implementation focuses on a complete prompt to tool proposal to approval or denial to tool result to response cycle.

This checkout was implemented and tested on macOS because no Haiku host or cross-development kit was available. The portable core, CLI, and deterministic tests pass there. Native compilation, window behavior, and responsiveness still require verification on Haiku; they are not claimed as complete.

## What is included

- `core/include/kairo` and `core/src`: provider-neutral messages and tool calls, typed events, cancellation, bounded agent loop, SSE decoder, OpenAI-compatible Chat Completions adapter, project-scoped tools, approvals, and versioned session persistence.
- `apps/cli`: a thin interactive client and deterministic `--fake` mode.
- `apps/gui`: a native `BApplication` and `BWindow` with a menu bar, project and session controls, model menu, labelled transcript and multiline prompt, Send, Cancel, status, and asynchronous approval alerts.
- `tests`: deterministic coverage for fragmented SSE input, fragmented tool arguments, approvals, denials, path escapes, cancellation, session resume, and restrictive session-file permissions.
- `Makefile`: builds with the installed C++17 compiler and enables libcurl by default, detecting its flags with `pkg-config` or `curl-config` before falling back to `-lcurl`. On Haiku it also builds the native GUI and links it with the Application Kit through `-lbe`.

## Architecture

`AgentEngine` owns the sequential model and tool loop. It receives a `Provider`, `Workspace`, and `SessionStore`; neither the engine nor those types know about `BWindow`. Providers return complete assistant messages and tool calls while publishing incremental text events. The engine publishes typed lifecycle events and records each tool result or denial as a tool message.

The Haiku window starts the engine on a worker thread. It sends copied event fields to the window with `BMessenger` and `BMessage`, and only the window looper updates views. Approval requests create asynchronous `BAlert` instances; the worker waits on the associated request, not the window looper. Cancellation is shared with the HTTP transfer and command runner. Closing the window cancels work, denies pending approvals, and prevents the worker from retaining a window pointer.

The OpenAI-compatible adapter uses `POST {base_url}/chat/completions` with streaming enabled. Its SSE decoder accepts arbitrary network fragmentation and assembles tool-call arguments by tool index. The CLI reads the API key from its configured environment variable. The GUI can instead read it from its saved provider settings. Credentials are not copied into requests stored on disk, logs, errors, or session files.

## Build prerequisites

The build needs:

- a C++17 compiler
- POSIX process APIs
- `make`
- libcurl development files for the live provider (`pkg-config` or `curl-config` is recommended for discovery)
- Haiku development headers plus the `be` and `tracker` libraries for the GUI

On Haiku, inspect the actual package names exposed by the configured repository before installing anything:

```sh
pkgman search curl_devel
pkgman search pkgconfig
```

The current HaikuPorts curl recipe provides `curl_devel`, `devel:libcurl`, `curl-config`, and pkg-config metadata. A typical setup is therefore:

```sh
pkgman install curl_devel
```

Haiku's package repositories and package naming can vary by architecture and release, so use the search results on the target system as the authority. References: [Haiku API documentation](https://www.haiku-os.org/docs/api/), [Haiku source](https://github.com/haiku/haiku), and [HaikuPorts curl recipe](https://github.com/haikuports/haikuports/tree/master/net-misc/curl).

## Build and test

```sh
make
make test
```

`make` builds `build/kairo-cli` everywhere. When `uname -s` reports `Haiku`, it also builds `build/kairo-gui`. To request the GUI target explicitly on Haiku:

```sh
make gui
```

The Haiku x86_64 GUI build uses the bundled libcurl 8.21.0 library and headers in `vendor/haiku-x86_64`. The build adds its `include` directory automatically, copies `libcurl.so`, `libcurl.so.4`, and `libcurl.so.4.8.0` beside `build/kairo-gui`, and gives the executable an `$ORIGIN` runtime search path. Curl's other runtime dependencies still come from the Haiku installation.

To rebuild everything:

```sh
make clean
make check
```

Libcurl is enabled by default. The build obtains its flags from `pkg-config`, then `curl-config`, and otherwise links with `-lcurl`. To deliberately make an offline build with the live adapter disabled, run `make KAIRO_USE_CURL=0`; the deterministic fake-provider path remains available.

## CLI

Set the endpoint, model, and key without putting the key on the command line:

```sh
export KAIRO_API_KEY='your key'
./build/kairo-cli --project /boot/home/src/example \
  --endpoint https://api.openai.com/v1 \
  --model qwen/qwen3.8-27b
```

The endpoint must be an OpenAI-compatible base URL whose Chat Completions route is at `/chat/completions`. Use `--key-env NAME` to select another environment variable. The program never prints its value.

Useful session commands:

```sh
./build/kairo-cli --list-sessions
./build/kairo-cli --project /boot/home/src/example --resume SESSION_ID
```

Sessions default to `~/config/settings/Kairo/sessions` on Haiku and `~/.local/share/kairo/sessions` elsewhere. Directories are set to mode `0700` and files to `0600`. The JSON schema has an explicit `version` field. Sessions include the project path, model, messages, tool calls, and results, but never credentials.

Use the deterministic provider to exercise approvals without a network connection or API key:

```sh
./build/kairo-cli --project /boot/home/src/example --fake
```

A prompt containing `read` proposes an automatic read of `README.md`; one containing `shell` proposes an approved shell command; other prompts propose an approved write to `kairo-demo.txt`.

## GUI configuration

The GUI keeps provider configuration in a separate **Settings > Provider Settings...** window. The same window is also available from the **Settings...** button beside the model selector. It uses these environment variables as initial or fallback values:

- `KAIRO_API_KEY`: required API key
- `KAIRO_BASE_URL`: optional compatible base URL; defaults to `https://api.openai.com/v1`

The project field defaults to `/boot/home/work`. Choose a project directory and a model (`qwen/qwen3.8-27b`, `openai/gpt-oss-120b`, `openai/gpt-oss-20b`, `gpt-4.1-mini`, `gpt-4.1`, `gpt-4.1-nano`, `o4-mini`, or `o3`) in the top controls; `qwen/qwen3.8-27b` is selected by default. Open Provider Settings to enter an OpenAI-compatible base URL and API key, then press Save. Kairo stores them in `~/config/settings/Kairo/provider_settings` on Haiku, with the Kairo settings directory restricted to mode `0700` and the file to `0600`. The key is masked in the settings window and is never written to session files or log output. Leave the session field blank to create a session, or paste a session ID to resume one for the same canonical project. When resuming, the model selected in the dropdown replaces the model saved in that session for the next request and subsequent saves. Kairo only fills the session field after the session has successfully been saved. The Send button is disabled while a run is active; Cancel signals both the provider transfer and any active shell child process.

The GUI writes a timestamped diagnostic log to `~/config/settings/Kairo/kairo.log`. The log file uses mode `0600` and records application, settings, and run lifecycle events without recording API keys, URLs, prompts, model responses, or tool output. Detailed run errors remain visible in the conversation window without copying provider response bodies into the log.

On Haiku, Kairo automatically uses `/boot/system/data/ssl/CARootCertificates.pem` when it is available. `CURL_CA_BUNDLE` or `SSL_CERT_FILE` can override the certificate file, and `SSL_CERT_DIR` can supply a certificate directory. If the system bundle is missing, install it with `pkgman install ca_root_certificates` before using an HTTPS provider. TLS peer verification is never disabled.

## Tools and safety policy

The initial tool set is intentionally small:

- `read_file`: automatic only after canonical-path validation inside the chosen project.
- `search_files`: automatic, recursive, literal search inside the chosen project; symlink escapes are skipped.
- `write_file`: canonical-path validation followed by a preview and per-action approval. The write uses a temporary sibling and rename.
- `run_shell`: shows the exact command and working directory and requires approval when the default approval mode is enabled. A denial never forks a process. Shell commands are not represented as path-safe merely because their working directory is inside the project.

Tool paths are literal project paths rather than shell expressions. Models should use `.` for the project root; Kairo rejects `$HOME`, other environment-variable expressions, and `~` instead of expanding them beyond the workspace boundary.

The text reader rejects binary files instead of sending ELF or other binary data back to the model. Native Haiku guidance steers the agent toward C++ `BApplication`/`BWindow` sources compiled with `g++` and linked with `-lbe`, without probing for obsolete BeOS-era utilities. The default agent iteration budget is 12.

The GUI transcript uses distinct colours and fonts for user prompts, assistant responses, tool names, fixed-width tool output, denials, and errors. This formatting uses the native stylable `BTextView` API.

The **Ask before file writes and shell commands** checkbox is enabled by default. When enabled, every mutating tool action requires its own approval. Turning it off auto-approves writes and shell commands for runs started while it is off; the checkbox is locked during a run and returns to the safe enabled state whenever Kairo restarts.
Disabling it also requires confirmation in a warning dialog.

There are no permanent grants. Tool output, total request content, and loop iterations are bounded. A denial is persisted as a normal tool result so the model can continue without pretending the action occurred.

Shell subprocesses receive a filtered environment: the configured API-key variable,
credential-shaped variables, authentication-agent sockets, and askpass helpers are
removed before execution. This prevents an approved model-generated command from
reading the provider key directly from the process environment. It does not sandbox
shell commands from other files readable by the current user, so command approval
remains the primary safety boundary.

## Verification performed on 25 September 2026

Host: Darwin arm64 with Apple clang 21.0.0, libcurl 8.7.1, and no CMake installation. The native Makefile was selected because it is sufficient for this small C++17 project and avoids adding a build dependency.

Commands run:

```text
make check
make clean && make check PKG_CONFIG=false
interactive kairo-cli --fake approval and denial smoke runs in a temporary project
sanitizer build plus localhost OpenAI-compatible SSE integration test with seven-byte network fragments
```

Result: the shared core library, CLI, and test executable compiled with warnings enabled both with and without libcurl; all deterministic and sanitizer tests passed. The CLI created an approved file, did not execute a denied shell command, and wrote resumable session files with mode `0600`. The live adapter completed a two-request tool loop against a local mock endpoint while every SSE response was split into seven-byte transport writes.

The tests establish that:

- fragmented SSE input is reconstructed correctly;
- fake-provider tool arguments assembled from fragments execute correctly;
- the engine completes the proposal, approval or denial, result, and final-response cycle;
- denied writes and shell commands have no side effects;
- `..` escapes are rejected and searched symlink escapes are skipped;
- cancellation produces a terminal cancellation event;
- sessions resume with their message history and are stored with restrictive permissions.

## Security review performed on 28 September 2026

The source tree was scanned for credential-shaped values and machine-specific
developer paths; no embedded credentials were found. Both curl-enabled and
curl-free builds passed with warnings enabled, and the test suite passed under
AddressSanitizer and UndefinedBehaviorSanitizer (with unsupported macOS leak
detection disabled). The review also added HTTPS enforcement for remote
providers, explicit TLS verification, protocol restrictions, bounded network and
JSON parsing, secure temporary files, shell-environment filtering, terminal
control-character filtering, and cancellation-safe GUI approval handling.

## Haiku verification still required

Run these steps on the target Haiku installation before calling the native release complete:

1. Record `uname -a`, `c++ --version`, `make --version`, `pkg-config --modversion libcurl`, and the installed `curl_devel` package version.
2. Run `make clean && make check` and resolve any compiler or API differences rather than weakening warnings.
3. Start `build/kairo-cli --project SAFE_PROJECT --fake`; approve one write, deny one write, approve a harmless shell command, and confirm a resumed session.
4. Start `build/kairo-gui` with a mock project. Verify streaming leaves window controls responsive, approval details are exact, denial has no side effects, Cancel ends a request, and closing during work produces no late UI update or crash.
5. Restart the GUI and resume the saved session. Confirm that no API key appears in its JSON file or error output.
6. If credentials are available, exercise the configured compatible endpoint in both CLI and GUI. Record the provider and model tested; live endpoint behavior has not been verified in this checkout.

Tracker references, MIME registration, filesystem attributes and queries, notifications, Deskbar integration, MCP, and multiple agents remain deliberately outside this first working slice.
