# Kairo technical notes

This document contains implementation, build, verification, and contributor
details. The main [README](../README.md) intentionally stays focused on installing
and using Kairo.

## Repository layout

- `core/include/kairo` and `core/src` contain the portable engine, provider
  adapters, workspace policy, events, cancellation, and session persistence.
- `apps/cli` contains the portable command-line client and deterministic fake
  provider mode.
- `apps/gui` contains the native Haiku `BApplication` and `BWindow` interface.
- `tests` contains deterministic core and protocol tests.
- `resources` contains the application metadata and icon sources.
- `scripts` contains resource-build helpers.
- `vendor/haiku-x86_64` contains the bundled x86_64 Haiku libcurl development and
  runtime files.

## Architecture

### API-provider engine

`AgentEngine` owns the sequential model and tool loop. It receives a `Provider`,
`Workspace`, and `SessionStore`; these types do not depend on `BWindow`.
Providers return assistant text and tool calls while publishing incremental
events. The engine executes proposed tools, records their results or denials, and
saves the session after each iteration.

The OpenAI-compatible transport calls `POST {base_url}/chat/completions` with
streaming enabled. Its SSE decoder accepts arbitrary network fragmentation and
assembles fragmented tool-call arguments by tool index. Anthropic profiles
currently use Anthropic's OpenAI SDK compatibility endpoint and include the
required token limit.

### ChatGPT subscription backend

Kairo must not depend on the Codex command-line program: it is not available on
Haiku, and Kairo exists to provide a native Haiku client. The previously explored
`codex app-server` adapter was removed for that reason.

The ChatGPT Plus and Pro integration uses an in-app **Continue with ChatGPT**
flow. Kairo dynamically registers as an open-source client, opens the authorization
page, receives the loopback callback, validates OAuth state and the signed ID token,
and retains rotating credentials in an atomic owner-only file. It discovers the
account's eligible models and calls the Responses API directly with `store:false`,
streaming enabled, full local context, and encrypted reasoning items preserved
across local tool calls. Kairo's local functions are grouped in a Responses tool
namespace as required by the ChatGPT-plan preview route. Access tokens are refreshed before expiry. Sign-out
attempts remote refresh-token revocation and always clears the local tokens.
References:

- [Sign in with ChatGPT quickstart](https://developers.openai.com/siwc/quickstart)
- [Open-source registration and sign-in](https://developers.openai.com/siwc/token-sharing-open-source/sign-in)
- [Models and direct inference](https://developers.openai.com/siwc/token-sharing-open-source/models-and-inference)
- [Token storage and refresh](https://developers.openai.com/siwc/token-sharing-open-source/token-reference)

### Haiku GUI threading

The Haiku window starts a run on a worker thread. Event fields are copied into
`BMessage` objects and sent with `BMessenger`; only the window looper updates
views. Approval requests use asynchronous `BAlert` objects while the worker waits
on the associated request. Closing the window cancels active work, denies pending
approvals, and joins the worker.

The GUI transcript uses native stylable `BTextView` runs to distinguish user
prompts, assistant text, tool names, fixed-width tool output, denials, and errors.

## Provider profiles and saved state

Provider profiles store an ID, display name, provider kind, API endpoint,
credential source, editable model list, and selected model. Existing settings are
migrated and newly supplied default profiles are merged by stable ID.

On Haiku, settings are saved under `~/config/settings/Kairo`. The directory uses
mode `0700`; settings, session, and log files use mode `0600`.

Sessions include:

- schema version and Kairo session ID;
- canonical project path;
- provider-profile ID and selected model;
- messages, tool results, and provider-specific continuation items.

Credentials are not stored in session files.

ChatGPT OAuth credentials are separate from provider profiles and sessions. The
credential record stores the stable host ID, issued dynamic-client ID, verified
account identity, retained ID token, access token, rotating refresh token, scopes,
and expiry under `~/config/settings/Kairo/chatgpt_credentials.json` with mode
`0600`. Provider settings and diagnostics never contain these tokens.

## Build details

The Makefile requires a C++17 compiler and POSIX process APIs. It enables libcurl
by default, discovers flags through `pkg-config` or `curl-config`, then falls back
to `-lcurl`. On Haiku, a GUI build also compiles and links a probe for the
OpenSSL 3 APIs used to validate ChatGPT tokens. The build fails with an actionable
dependency error instead of producing a GUI with ChatGPT sign-in compiled out.

Useful targets:

```sh
make                 # portable CLI; native GUI too when uname reports Haiku
make gui             # native Haiku GUI
make test            # deterministic test executable
make check           # build and test
make clean
```

An offline core build without the live HTTP adapter is available with:

```sh
make KAIRO_USE_CURL=0
```

### Bundled Haiku libcurl

The tested HaikuPorts libcurl package was older than the version required by this
checkout. On Haiku x86_64 the Makefile adds `vendor/haiku-x86_64/include`, links
against `vendor/haiku-x86_64/lib`, copies the versioned shared libraries beside
the GUI executable, and sets an `$ORIGIN` runtime search path. Other curl runtime
dependencies are supplied by Haiku.

The build links the GUI with `-lbe -ltracker`. It uses `hvif_tools`, when present,
to convert and embed the application icon and always attempts to embed the
application signature and version metadata. All Haiku executables link with
`libnetwork` for the OAuth loopback listener and use the bundled libcurl search
path and `$ORIGIN` runtime path consistently.

## CLI reference

Configure an OpenAI-compatible provider without placing its key on the command
line:

```sh
export KAIRO_API_KEY='your key'
./build/kairo-cli --project /boot/home/src/example \
  --endpoint https://api.openai.com/v1 \
  --model MODEL_ID
```

Use another environment-variable name with `--key-env NAME`.

Session commands:

```sh
./build/kairo-cli --list-sessions
./build/kairo-cli --project /boot/home/src/example --resume SESSION_ID
```

Exercise approvals without a network connection:

```sh
./build/kairo-cli --project /boot/home/src/example --fake
```

A fake prompt containing `read` proposes an automatic `README.md` read; one
containing `shell` proposes a shell command; other prompts propose a write to
`kairo-demo.txt`.

## Security boundaries

Built-in workspace paths are literal project paths. `$HOME`, `~`, and environment
expressions are rejected rather than expanded, canonical-path checks reject
escapes, and searches skip symlinks leaving the project. The text reader rejects
binary files.

Writes use a temporary sibling followed by rename. Shell commands receive a
filtered environment that removes the configured key variable, credential-shaped
variables, authentication-agent sockets, and askpass helpers.

This is not a complete operating-system sandbox. An approved shell command runs
with the current user's file permissions, so user review remains a primary safety
boundary. See [SECURITY.md](../SECURITY.md) for reporting guidance.

## Automated verification

The portable suite covers:

- fragmented and multiline SSE input;
- fragmented tool-call arguments;
- approval, denial, cancellation, and session resume;
- project path and symlink escapes;
- binary-file rejection;
- restrictive settings and session permissions;
- provider URL policy and TLS requirements;
- ChatGPT provider selection and API-key separation;
- removal of credential-shaped environment variables from child processes; and
- bounded JSON, request, response, and tool output handling.

On 2 October 2026 the portable project passed:

```text
make check
make clean && make check KAIRO_USE_CURL=0
AddressSanitizer and UndefinedBehaviorSanitizer test build
git diff --check
credential-pattern source scan
```

These checks were run on macOS. They do not replace native Haiku GUI testing.

## Native Haiku release checklist

Before calling a release complete on Haiku:

1. Record `uname -a`, `c++ --version`, `make --version`, and the installed curl
   package versions.
2. Run `make clean && make check`.
3. Run the CLI fake-provider flow; approve and deny writes, approve a harmless
   command, then resume the session.
4. Start `build/kairo-gui` and verify provider/model switching, settings restart,
   streamed text, approval details, denial, cancellation, and close-during-run.
5. Confirm that the icon, signature, and version resources are embedded.
6. Inspect a saved session and log to confirm that no credential appears.
7. Test one live API provider and record the provider and exact model ID.

## Known limitations and future work

- The native GUI, HVIF conversion, and resource embedding still require repeated
  testing on actual Haiku releases.
- The ChatGPT OAuth and Responses path still needs final end-to-end verification
  on a native Haiku installation with an eligible account. No external Codex
  executable is needed or supported.
- Anthropic currently uses its compatibility endpoint rather than a native
  Messages API transport.
- Tracker references, filesystem attributes and queries, notifications, Deskbar
  integration, MCP, and multiple agents are outside the current working slice.
