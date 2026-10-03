# Kairo for Haiku

Kairo is a native Haiku coding assistant. It has a Haiku `BApplication` interface,
project-scoped file and shell tools, approval prompts, saved conversations, and a
choice of AI providers.

> Kairo is under active development. The portable core and automated tests are
> exercised on macOS, but every native GUI release still needs final testing on
> Haiku.

## Download the source

Open Terminal on Haiku. If the `git` command is missing, install it first with
`pkgman install git`. Then run:

```sh
cd /boot/home
git clone https://github.com/sikosis/kairo.git Kairo
cd Kairo
```

To update an existing checkout later:

```sh
cd /boot/home/Kairo
git pull
```

## Install the build requirements

Kairo needs Git, a C++17 compiler, `make`, and the standard Haiku development
files. OpenSSL development headers are used to validate ChatGPT identity tokens.
The x86_64 repository checkout already contains the libcurl headers and main
shared library used by the build.

If Git or the curl runtime dependencies are missing, inspect the package names
available on your Haiku release:

```sh
pkgman search git
pkgman search curl
pkgman search hvif_tools
```

A typical setup is:

```sh
pkgman install git
pkgman install curl
pkgman install ca_root_certificates
pkgman install devel:libcrypto pkgconfig
pkgman install hvif_tools
```

`hvif_tools` is required for GUI builds so Kairo's application icon is always
embedded for the About box, Tracker, and Deskbar. OpenSSL 3 is also required:
`make gui` stops with an installation command if either dependency is missing.

### Note about HaikuPorts libcurl

The libcurl package in the HaikuPorts repository used during development was too
old for this project. The x86_64 checkout therefore includes the required libcurl
headers and shared libraries under `vendor/haiku-x86_64`. The build copies the
libraries beside `build/kairo-gui` and configures the executable to load them from
that directory. Curl's other dependencies still come from Haiku.

## Build Kairo

From the repository directory:

```sh
make gui
```

The application will be created at:

```text
/boot/home/Kairo/build/kairo-gui
```

Run it with:

```sh
./build/kairo-gui
```

If an older build displays “built without OpenSSL support,” install the missing
development package and completely rebuild it:

```sh
pkgman install devel:libcrypto pkgconfig
make clean
make gui
```

The new compile commands must contain `-DKAIRO_HAS_CRYPTO=1`. If they do not,
check Haiku's package metadata with `pkg-config --cflags --libs libcrypto`.

To build everything and run the automated tests:

```sh
make clean
make check
```

## First-time setup

1. Start `build/kairo-gui`.
2. Leave **Project** as `/boot/home/work`, or choose another project directory.
3. Open **Settings > Provider Settings...**.
4. Select or add a provider.
5. Enter its API key and API URL if it is an API provider.
6. Save the settings, choose a model, enter a prompt, and press **Send**.

Provider settings are saved in:

```text
~/config/settings/Kairo/provider_settings
```

The settings directory is restricted to the current user. When practical, use a
key environment-variable name and leave the saved API-key field empty.

## Providers

Kairo includes starting profiles for:

- **OpenAI API**
- **Anthropic API compatibility**
- **OpenRouter or another OpenAI-compatible service**
- **ChatGPT Plus / Pro** through **Continue with ChatGPT**

Provider model lists are editable because model access differs by account. Enter
the exact model ID supplied by your provider.

### Using an API provider

Open **Provider Settings**, choose the provider, then enter:

- the API base URL;
- either an API key or the name of an environment variable containing it; and
- one or more model IDs, one per line.

For example, an OpenRouter-compatible setup uses an API base URL such as
`https://openrouter.ai/api/v1` and the exact OpenRouter model ID.

### ChatGPT subscriptions

Kairo does not require, launch, or wrap the Codex command-line program. There is
no Codex executable for Haiku, and supplying one would defeat Kairo's purpose.

Open **Provider Settings**, select **ChatGPT Plus / Pro**, and press **Continue
with ChatGPT**. Kairo receives the authorization on a temporary `127.0.0.1`
callback and loads the models available to that account. It copies the sign-in URL
to the clipboard and normally opens the system browser. When WebPositive is the
default, Kairo deliberately leaves it closed and asks you to paste the URL into
Firefox because WebPositive cannot reliably complete OpenAI's current security
check. Keep Provider Settings open until it reports that the account is connected.
Kairo saves the discovered model list after sign-in; choose one in the main window.

This flow uses OpenAI's OAuth support for open-source applications and calls the
Responses API directly. It does not require an API key or an external Codex
installation. Kairo stores the rotating tokens in the owner-only file
`~/config/settings/Kairo/chatgpt_credentials.json`; **Sign Out** revokes the
renewable session and clears the local tokens while retaining the verified account,
host, and client registration mapping needed for a later sign-in.

OpenAI currently describes ChatGPT-plan access as a preview. Model and plan usage
availability therefore depends on the selected account, workspace, region, and
OpenAI policy; Kairo reports those service errors without falling back to API billing.

## Safety and approvals

**Ask before file writes and shell commands** is enabled by default. Keep it
enabled unless you deliberately want the selected model to make project changes
without individual confirmation.

Kairo confines its built-in tools to the selected project, rejects path escapes,
filters credential-shaped environment variables from child processes, and never
writes API keys into conversation files or diagnostic logs. Approved shell
commands still run with your user account, so read the command shown in every
approval dialog.

## Diagnostic log

Kairo writes an owner-only diagnostic log to:

```text
~/config/settings/Kairo/kairo.log
```

It records the Kairo version, provider kind, selected model, project and session,
approval state, ChatGPT sign-in stages, tool start/finish status, and sanitized
errors. It deliberately does not record prompts, model replies, tool output, API
keys, OAuth URLs, or tokens. Project paths and session IDs can still reveal local
information, so inspect the file before sharing it publicly.

## Common problems

### `curl/curl.h: No such file or directory`

Make sure this is a complete x86_64 checkout containing
`vendor/haiku-x86_64/include/curl/curl.h`. If that file is missing, restore it
from Git or clone a fresh copy:

```sh
cd /boot/home/Kairo
git status
git restore vendor/haiku-x86_64
make clean
make gui
```

### SSL certificate error

Install Haiku's certificate bundle:

```sh
pkgman install ca_root_certificates
```

Kairo automatically checks `/boot/system/data/ssl/CARootCertificates.pem`.

### The provider rejects a model

Open provider settings and replace the model with an exact model ID available to
your account. A model appearing in a general catalogue does not guarantee that
the selected account can use it.

### ChatGPT sign-in stalls in WebPositive

WebPositive may be unable to complete OpenAI's current Cloudflare browser check.
When it is the default browser, Kairo copies the short-lived sign-in URL without
opening WebPositive. Paste that URL into Firefox, finish signing in, and keep
Provider Settings open so Kairo can receive the redirect on `127.0.0.1`. The
listener remains available until sign-in completes or you explicitly cancel it,
and ignores unrelated or malformed local requests instead of abandoning the
sign-in attempt.

## More documentation

- [Technical details and contributor notes](docs/TECHNICAL.md)
- [Security policy](SECURITY.md)
- [Application icon workflow](resources/README.md)
- [Bundled libcurl origin, dependencies, and security status](vendor/haiku-x86_64/README.md)
- [Haiku API documentation](https://www.haiku-os.org/docs/api/)
- [Be Book](https://www.haiku-os.org/legacy-docs/bebook/index.html)

## License

Kairo is released under the [MIT License](LICENSE).
