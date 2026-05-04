# SPDX-License-Identifier: LGPL-3.0-or-later
"""Pythonic facade for runtime MCP server registration (#8, v2.1.4).

The C ABI ``entropic_register_mcp_server(handle, name, config_json)``
takes a JSON string. This module wraps it with kwargs + dict→JSON
conversion + validation, so consumers get type-checked argument
construction and don't have to hand-build JSON.

Issue #9 (v2.1.4) on the C side: env keys are filtered through the
.mcp.json blocklist (PATH, LD_PRELOAD, etc. cannot be injected). This
module passes env through verbatim — relies on the C-side filter.

Usage
-----

::

    import entropic

    handle = entropic.create()
    entropic.configure(handle, ...)
    entropic.register_server(
        handle,
        name="docs",
        command="/usr/local/bin/docs-mcp",
        args=["--mode=server"],
        env={"DOCS_DB": "/srv/docs.db"},
    )

@version 2.1.4
"""

from __future__ import annotations

import json
from collections.abc import Mapping, Sequence

from entropic._bindings import entropic_register_mcp_server


## @brief Register an external MCP server at runtime via the C ABI.
## @utility
## @version 2.1.4
def register_server(  # noqa: CFQ002 (intentional kwargs-only API)
    handle,
    name: str,
    *,
    command: str | None = None,
    args: Sequence[str] | None = None,
    env: Mapping[str, str] | None = None,
    url: str | None = None,
    transport: str | None = None,
) -> int:
    """Register an external MCP server with ``handle``.

    Exactly one of ``command`` / ``url`` must be provided (stdio vs
    SSE transport). The wire-format JSON the C ABI expects is built
    from kwargs.

    Args:
        handle: An entropic_handle_t from entropic_create.
        name: Unique server name (becomes the tool prefix, e.g. "docs"
              for tools "docs.lookup", "docs.search", ...).
        command: Stdio executable path (mutually exclusive with url).
        args: Stdio command arguments. Default: empty.
        env: Environment variables to set in the spawned child. The
             C-side blocklist (is_blocked_env_var) drops PATH /
             LD_PRELOAD / etc. silently with a warning.
        url: SSE endpoint (mutually exclusive with command).
        transport: Explicit "stdio" | "sse". Inferred from
                   command/url if absent.

    Returns:
        Engine return code (0 on success).

    Raises:
        ValueError: If neither or both of command/url are provided.

    @version 2.1.4
    """
    if (command is None) == (url is None):
        raise ValueError(
            "register_server: exactly one of command/url required "
            f"(got command={command!r}, url={url!r})"
        )

    config = {
        "command": command or "",
        "args": list(args or []),
        "env": dict(env or {}),
        "url": url or "",
        "transport": transport or ("sse" if url else "stdio"),
    }
    config_json = json.dumps(config).encode("utf-8")
    name_bytes = name.encode("utf-8")
    return int(entropic_register_mcp_server(handle, name_bytes, config_json))
