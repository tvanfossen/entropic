# Security Policy

## Supported Versions

| Version | Supported |
|---------|-----------|
| 2.1.x   | Yes — current release |
| 2.0.x   | Best-effort fixes for critical issues |
| 1.7.x   | PyPI fallback, frozen — no security backports |
| < 1.7   | No |

## Reporting a Vulnerability

If you discover a security vulnerability in Entropic, please report it
responsibly. **Do not open a public issue.**

Open a [private security advisory](https://github.com/tvanfossen/entropic/security/advisories/new)
on GitHub. Include a description of the vulnerability, steps to reproduce, and
any relevant logs or screenshots. You will receive an acknowledgment within
72 hours.

## Scope

Entropic is a local inference engine. The following are in scope:

- Arbitrary code execution via tool servers (filesystem, bash, diagnostics)
- Privilege escalation through MCP tool approval bypass
- Prompt injection that circumvents tool approval controls
- Denial of service through crafted model inputs or configurations
- Path traversal in filesystem tool operations

The following are **out of scope**:

- Vulnerabilities in upstream dependencies (llama.cpp, nlohmann::json,
  spdlog, sqlite3, ryml, etc.) — report these to the respective projects
- Model behavior (bias, hallucination, unsafe outputs) — these are properties
  of the loaded model, not the engine
- Issues requiring physical access to the host machine

## Network serving (v2.14.0)

`mcp.external.http_port` (and `entropic serve --port N`) serves the external
MCP bridge at `http://127.0.0.1:N/mcp`. The security bar:

| Property | How |
|---|---|
| Never leaves the host | Binds `127.0.0.1` only; no TLS, no bind-address option. Remote access: `ssh -L N:127.0.0.1:N host`, which supplies authentication and encryption. |
| Bearer token | 256 bits from `getrandom()`, regenerated every start, written atomically to `~/.entropic/serve/N.token` (mode 0600). Removed on clean shutdown. |
| Token directory checked, not assumed | `~/.entropic/serve` must be 0700, owned by the engine's uid, and not a symlink, or the listener refuses to start. |
| Comparison and secrecy | Constant-time comparison; the token is never logged or echoed. |
| Browser / DNS rebinding | Any request with an `Origin` header is refused (403), as is a `Host` other than `127.0.0.1:N` / `localhost:N`. |
| Abuse limits | Body and header size caps, read/write timeouts; five failed authentications in a minute block all requests for 30 s, each failure logged without the token. |

Anyone who can read your home directory as your uid can read the token —
the same trust boundary as the bridge's unix socket.
