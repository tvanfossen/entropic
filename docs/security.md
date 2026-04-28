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
