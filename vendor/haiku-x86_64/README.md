# Bundled libcurl for Haiku x86_64

This directory contains the Haiku x86_64 build of libcurl 8.21.0 from the
HaikuPorts `curl-8.21.0-1-x86_64.hpkg` package and its public headers from
the matching `curl_devel-8.21.0-1-x86_64.hpkg` package.

- Package mirror: `https://mirror.truenetwork.ru/haiku/haikuports/x86_64/current/packages/curl-8.21.0-1-x86_64.hpkg`
- HPKG SHA-256: `75e6d43a09716a589e1451b404bc6d21dab2d101ccff7646a80f2ebe82a5b0d6`
- Development HPKG SHA-256: `f139553aa51167711a84e34f1d2bd17f376c671441e2663baa1f3dc7305b3e3a`
- Extracted `libcurl.so.4.8.0` SHA-256: `229a9174ed157711d55de36276eae751d95a6fe4dc42157b5e1f00747c175ee0`
- Library architecture: ELF 64-bit x86-64
- SONAME: `libcurl.so.4`
- Curl license: `LICENSE.libcurl`

The library depends on `libnghttp2.so.14`, `libssh2.so.1`, `libpsl.so.5`,
`libssl.so.3`, `libcrypto.so.3`, `libz.so.1`, `libnetwork.so`, and
`libroot.so`. These remain system/package dependencies; they are already
present when Haiku's curl runtime is installed.

On Haiku, the Makefile adds `include/` to the compiler's header search path.
`make gui` copies the linker symlink, versioned library, and SONAME symlink
next to `build/kairo-gui`. The executable is linked with an `$ORIGIN` runtime
path so it loads that adjacent copy.

## Security status

As of 28 September 2026, curl lists nine published vulnerabilities affecting
8.21.0 and recommends upgrading to 8.22.0. HaikuPorts' current x86_64 repository
still provides 8.21.0, so no matching trusted 8.22.0 HPKG is available to bundle.

Kairo constrains this library to the easy interface, HTTP/HTTPS, HTTP/1.1, a
fresh connection per request, explicit TLS peer and hostname verification, no
`.netrc`, no cookies, no certificate pinning, no Negotiate authentication, and
no HTTP/2 server push. Those constraints avoid the affected feature paths
described by curl's 8.21.0 advisories. Replace both the runtime library and
headers with the matching HaikuPorts 8.22.0-or-later packages as soon as they
are published; do not substitute a binary built for another operating system.
