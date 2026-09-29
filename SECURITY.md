# Security policy

## Reporting a vulnerability

Please do not open a public issue for a vulnerability or an exposed credential.
Use GitHub's private vulnerability reporting feature for the repository. If that
feature is not yet enabled, contact the maintainer privately before disclosing
details. Include reproduction steps, the affected version, and the practical
impact where possible.

If a real API key is ever committed, revoke it at the provider immediately. A
later deletion or history rewrite does not make a published credential safe to
reuse.

## Credential and local-data handling

- Kairo accepts an API key from its provider settings or `KAIRO_API_KEY`.
- API keys must never be committed, placed in example files, or written to logs.
- Local provider settings containing a key must be readable only by the user.
  They are local configuration, not encrypted secret storage.
- Session files can contain prompts, model responses, tool output, project paths,
  and excerpts of source files. Treat the session directory and `kairo.log` as
  potentially sensitive local data.
- Remote provider URLs must use HTTPS. Plain HTTP is accepted only for loopback
  providers (`localhost`, `127.0.0.1`, or `::1`).

## Tool safety

Kairo can write project files and run shell commands selected by a model. Keep
interactive approval enabled, inspect the exact target or command, and run Kairo
as an unprivileged user. A selected project directory is a trust boundary for
file tools, but an approved shell command has the same access as the Kairo
process itself. Kairo removes credential-shaped environment variables (and the
configured API-key variable) from shell subprocesses, but an approved command
can still read any file that the user account can read.

## Dependency updates

The Haiku x86_64 distribution includes a pinned libcurl binary. Its version,
origin, license, runtime dependencies, and checksums are recorded under
`vendor/haiku-x86_64/`. Review curl and OpenSSL security advisories before each
release and replace the bundle when its upstream package receives a security
update. The currently bundled 8.21.0 package has published advisories; Kairo
explicitly disables their affected protocol and connection-reuse paths, but the
bundle should still be replaced with HaikuPorts 8.22.0 or later when available.
