# SPDX-License-Identifier: LGPL-3.0-or-later
"""Test MCP server — minimal echo tool for headless validation.

Provides a single 'echo' tool that returns its input verbatim. Used
by the headless example to validate that .mcp.json auto-discovery
works without any code in the consumer.

@brief Minimal echo MCP server (stdio transport).
@version 1
"""

from __future__ import annotations

import json
import sys
from typing import Any

_TOOLS: list[dict[str, Any]] = [
    {
        "name": "echo",
        "description": "Echo the input string back verbatim. Used for testing.",
        "inputSchema": {
            "type": "object",
            "properties": {"message": {"type": "string", "description": "Text to echo"}},
            "required": ["message"],
        },
    },
]


## @brief Read one JSON-RPC line from stdin.
## @return Parsed dict or None on EOF.
## @utility
## @version 1
def _read() -> dict[str, Any] | None:
    """Read one JSON-RPC 2.0 message from stdin.

    @brief Read one JSON-RPC line from stdin.
    @version 1
    """
    line = sys.stdin.readline()
    if not line:
        return None
    return json.loads(line.strip())


## @brief Write one JSON-RPC line to stdout.
## @param msg Response dict.
## @utility
## @version 1
def _write(msg: dict[str, Any]) -> None:
    """Write one JSON-RPC 2.0 message to stdout.

    @brief Write one JSON-RPC line to stdout.
    @version 1
    """
    sys.stdout.write(json.dumps(msg, separators=(",", ":")) + "\n")
    sys.stdout.flush()


## @brief Dispatch a single JSON-RPC request.
## @param req Request dict.
## @utility
## @version 1
def _dispatch(req: dict[str, Any]) -> None:
    """Route an incoming request to the appropriate handler.

    @brief Dispatch a single JSON-RPC request.
    @version 1
    """
    method = req.get("method", "")
    rid = req.get("id")
    params = req.get("params", {})

    if method == "initialize":
        _write(
            {
                "jsonrpc": "2.0",
                "id": rid,
                "result": {
                    "protocolVersion": "2024-11-05",
                    "serverInfo": {"name": "test"},
                    "capabilities": {},
                },
            }
        )
    elif method == "tools/list":
        _write({"jsonrpc": "2.0", "id": rid, "result": {"tools": _TOOLS}})
    elif method == "tools/call":
        msg = params.get("arguments", {}).get("message", "")
        _write(
            {
                "jsonrpc": "2.0",
                "id": rid,
                "result": {
                    "content": [{"type": "text", "text": f"echo: {msg}"}],
                },
            }
        )
    else:
        _write(
            {
                "jsonrpc": "2.0",
                "id": rid,
                "error": {
                    "code": -32601,
                    "message": f"Unknown method: {method}",
                },
            }
        )


## @brief Main stdio loop.
## @utility
## @version 1
def serve() -> None:
    """Read JSON-RPC requests from stdin until EOF.

    @brief Main stdio loop.
    @version 1
    """
    while True:
        req = _read()
        if req is None:
            break
        _dispatch(req)


if __name__ == "__main__":
    serve()
