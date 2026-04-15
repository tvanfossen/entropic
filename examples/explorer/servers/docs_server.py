# SPDX-License-Identifier: LGPL-3.0-or-later
"""Docs MCP server — query doxygen SQLite database over stdio JSON-RPC.

Provides tools for looking up functions, classes, searching documentation,
listing files, and exploring inheritance hierarchies. All queries run
against the doxygen-generated entropic_docs.db SQLite database.

Registered with the entropic engine as an external MCP server; the C engine
communicates with it over stdio transport.

@brief Doxygen SQLite MCP server (stdio transport).
@version 1
"""

from __future__ import annotations

import json
import os
import sqlite3
import sys
from typing import Any

DB_PATH = os.environ.get(
    "ENTROPIC_DOCS_DB",
    os.path.join(os.path.dirname(__file__), "..", "..", "..", "docs", "entropic_docs.db"),
)

_MCP_TOOLS: list[dict[str, Any]] = [
    {
        "name": "lookup_function",
        "description": "Look up a function by name. Returns brief, params, return type, file:line, version.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {"type": "string", "description": "Function name (partial match supported)"}
            },
            "required": ["name"],
        },
    },
    {
        "name": "lookup_class",
        "description": "Look up a class or struct by name. Returns brief, members, inheritance, file.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {
                    "type": "string",
                    "description": "Class or struct name (partial match supported)",
                }
            },
            "required": ["name"],
        },
    },
    {
        "name": "search",
        "description": "Search documentation by keywords. Words are matched individually — 'engine loop state' finds entries containing any of those words. Returns top matches ranked by relevance.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "query": {"type": "string", "description": "Search query"},
                "limit": {
                    "type": "integer",
                    "description": "Max results (default 10)",
                    "default": 10,
                },
            },
            "required": ["query"],
        },
    },
    {
        "name": "list_files",
        "description": "List documented source files matching a pattern.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "pattern": {
                    "type": "string",
                    "description": "Glob-style pattern (e.g. 'src/core/*')",
                }
            },
            "required": ["pattern"],
        },
    },
    {
        "name": "get_hierarchy",
        "description": "Get the inheritance hierarchy for a class.",
        "inputSchema": {
            "type": "object",
            "properties": {"class_name": {"type": "string", "description": "Class name"}},
            "required": ["class_name"],
        },
    },
]


## @brief Open a read-only connection to the doxygen database.
## @return SQLite connection.
## @utility
## @version 1
def _get_db() -> sqlite3.Connection:
    """Open the doxygen SQLite database in read-only mode.

    @brief Open a read-only connection to the doxygen database.
    @version 1
    """
    conn = sqlite3.connect(f"file:{DB_PATH}?mode=ro", uri=True)
    conn.row_factory = sqlite3.Row
    return conn


## @brief Look up function(s) by name with params and file location.
## @param name Function name (supports partial match via LIKE).
## @return Formatted text with function details.
## @utility
## @version 1
def _lookup_function(name: str) -> str:
    """Query memberdef for functions matching the given name.

    @brief Look up function(s) by name with params and file location.
    @version 1
    """
    conn = _get_db()
    rows = conn.execute(
        """
        SELECT m.name, m.briefdescription, m.detaileddescription,
               m.type, m.argsstring, m.definition, m.line, m.scope,
               p.name as file_path
        FROM memberdef m
        JOIN path p ON m.file_id = p.rowid
        WHERE m.kind = 'function' AND m.name LIKE ?
        ORDER BY m.name
        LIMIT 20
        """,
        (f"%{name}%",),
    ).fetchall()
    conn.close()

    if not rows:
        return f"No functions found matching '{name}'."

    results: list[str] = []
    for row in rows:
        parts = [f"### {row['name']}"]
        if row["briefdescription"]:
            parts.append(_clean_doxy(row["briefdescription"]))
        if row["definition"]:
            parts.append(f"```c\n{row['definition']}{row['argsstring'] or ''}\n```")
        parts.append(f"**File:** {row['file_path']}:{row['line']}")
        if row["scope"]:
            parts.append(f"**Scope:** {row['scope']}")
        if row["detaileddescription"]:
            parts.append(f"\n**Details:**\n{_clean_doxy(row['detaileddescription'])}")
        results.append("\n".join(parts))

    return "\n\n---\n\n".join(results)


## @brief Format a single compound (class/struct) with members and inheritance.
## @param conn SQLite connection.
## @param compound Row from compounddef query.
## @return Formatted markdown string for the compound.
## @utility
## @version 1
def _format_compound(conn: sqlite3.Connection, compound: sqlite3.Row) -> str:
    """Build markdown for one class/struct including members and hierarchy.

    @brief Format a single compound with members and inheritance.
    @version 1
    """
    parts = [f"### {compound['name']} ({compound['kind']})"]
    if compound["briefdescription"]:
        parts.append(_clean_doxy(compound["briefdescription"]))
    parts.append(f"**File:** {compound['file_path']}:{compound['line']}")

    members = conn.execute(
        """
        SELECT md.name, md.kind, md.briefdescription
        FROM member m
        JOIN memberdef md ON m.memberdef_rowid = md.rowid
        WHERE m.scope_rowid = ?
        ORDER BY md.kind, md.name
        """,
        (compound["rowid"],),
    ).fetchall()

    if members:
        parts.append("\n**Members:**")
        for mem in members:
            brief = _clean_doxy(mem["briefdescription"]) if mem["briefdescription"] else ""
            suffix = f": {brief}" if brief else ""
            parts.append(f"- `{mem['name']}` ({mem['kind']}){suffix}")

    _append_inheritance(conn, compound["rowid"], parts)
    return "\n".join(parts)


## @brief Append base and derived class info to parts list.
## @param conn SQLite connection.
## @param rowid Compound rowid.
## @param parts Accumulator list to append to.
## @utility
## @version 1
def _append_inheritance(
    conn: sqlite3.Connection,
    rowid: int,
    parts: list[str],
) -> None:
    """Query and append inheritance relationships for a compound.

    @brief Append base and derived class info to parts list.
    @version 1
    """
    bases = conn.execute(
        """
        SELECT c.name FROM compoundref cr
        JOIN compounddef c ON cr.base_rowid = c.rowid
        WHERE cr.derived_rowid = ?
        """,
        (rowid,),
    ).fetchall()

    if bases:
        parts.append(f"\n**Inherits from:** {', '.join(b['name'] for b in bases)}")

    derived = conn.execute(
        """
        SELECT c.name FROM compoundref cr
        JOIN compounddef c ON cr.derived_rowid = c.rowid
        WHERE cr.base_rowid = ?
        """,
        (rowid,),
    ).fetchall()

    if derived:
        parts.append(f"**Derived classes:** {', '.join(d['name'] for d in derived)}")


## @brief Look up class or struct by name with members and inheritance.
## @param name Class name (supports partial match via LIKE).
## @return Formatted text with class details.
## @utility
## @version 1
def _lookup_class(name: str) -> str:
    """Query compounddef for classes/structs matching the given name.

    @brief Look up class or struct by name with members and inheritance.
    @version 1
    """
    conn = _get_db()
    compounds = conn.execute(
        """
        SELECT c.rowid, c.name, c.kind, c.briefdescription,
               c.detaileddescription, c.line, p.name as file_path
        FROM compounddef c
        JOIN path p ON c.file_id = p.rowid
        WHERE c.kind IN ('class', 'struct') AND c.name LIKE ?
        ORDER BY c.name
        LIMIT 10
        """,
        (f"%{name}%",),
    ).fetchall()

    if not compounds:
        conn.close()
        return f"No classes or structs found matching '{name}'."

    results = [_format_compound(conn, c) for c in compounds]
    conn.close()
    return "\n\n---\n\n".join(results)


## @brief Format compound search results as markdown list.
## @param compounds List of compound rows from search query.
## @return List of formatted lines.
## @utility
## @version 1
def _format_compound_results(compounds: list[sqlite3.Row]) -> list[str]:
    """Format compound (class/struct/namespace) search results.

    @brief Format compound search results as markdown list.
    @version 1
    """
    lines = ["## Types"]
    for c in compounds:
        brief = _clean_doxy(c["briefdescription"]) if c["briefdescription"] else ""
        lines.append(f"- **{c['name']}** ({c['kind']}) — {c['file_path']}:{c['line']}")
        if brief:
            lines.append(f"  {brief}")
    return lines


## @brief Format memberdef search results as markdown list.
## @param members List of memberdef rows from search query.
## @return List of formatted lines.
## @utility
## @version 1
def _format_member_results(members: list[sqlite3.Row]) -> list[str]:
    """Format memberdef (function/variable/etc.) search results.

    @brief Format memberdef search results as markdown list.
    @version 1
    """
    lines = ["\n## Members"]
    for f in members:
        brief = _clean_doxy(f["briefdescription"]) if f["briefdescription"] else ""
        scope = f"  [{f['scope']}]" if f["scope"] else ""
        lines.append(f"- **{f['name']}** ({f['kind']}) — {f['file_path']}:{f['line']}{scope}")
        if brief:
            lines.append(f"  {brief}")
    return lines


## @brief Tokenize query and build SQL WHERE clause matching any token.
## @param tokens List of search tokens.
## @param columns Column names to search.
## @return Tuple of (WHERE clause string, parameter list).
## @utility
## @version 2
def _build_token_where(
    tokens: list[str],
    columns: list[str],
) -> tuple[str, list[str]]:
    """Build WHERE clause that matches any token in any column.

    @brief Tokenize query and build SQL WHERE clause matching any token.
    @version 2
    """
    clauses: list[str] = []
    params: list[str] = []
    for token in tokens:
        col_clauses = [f"{col} LIKE ?" for col in columns]
        clauses.append(f"({' OR '.join(col_clauses)})")
        params.extend([f"%{token}%"] * len(columns))
    return " OR ".join(clauses), params


## @brief Search functions and classes by keyword tokens.
## @param query Search query (tokenized into words, any match).
## @param limit Maximum number of results.
## @return Formatted text with search results.
## @utility
## @version 2
def _search(query: str, limit: int = 10) -> str:
    """Search across memberdef and compounddef using tokenized matching.

    @brief Search functions and classes by keyword tokens.
    @version 2
    """
    tokens = [t for t in query.lower().split() if len(t) >= 2]
    if not tokens:
        return f"No results found for '{query}'."

    conn = _get_db()
    member_cols = ["m.name", "m.briefdescription", "m.detaileddescription"]
    where, params = _build_token_where(tokens, member_cols)

    functions = conn.execute(
        f"""
        SELECT m.name, m.briefdescription, m.kind, m.line,
               p.name as file_path, m.scope
        FROM memberdef m
        JOIN path p ON m.file_id = p.rowid
        WHERE {where}
        ORDER BY m.name
        LIMIT ?
        """,
        [*params, limit],
    ).fetchall()

    compound_cols = ["c.name", "c.briefdescription", "c.detaileddescription"]
    where_c, params_c = _build_token_where(tokens, compound_cols)

    compounds = conn.execute(
        f"""
        SELECT c.name, c.briefdescription, c.kind, c.line,
               p.name as file_path
        FROM compounddef c
        JOIN path p ON c.file_id = p.rowid
        WHERE c.kind IN ('class', 'struct', 'namespace')
              AND ({where_c})
        ORDER BY c.name
        LIMIT ?
        """,
        [*params_c, limit],
    ).fetchall()
    conn.close()

    results: list[str] = []
    if compounds:
        results.extend(_format_compound_results(compounds))
    if functions:
        results.extend(_format_member_results(functions))

    if not results:
        return f"No results found for '{query}'."

    return "\n".join(results)


## @brief List documented files matching a glob-like pattern.
## @param pattern Pattern with * wildcards (e.g. 'src/core/*').
## @return Formatted text with file list and brief descriptions.
## @utility
## @version 1
def _list_files(pattern: str) -> str:
    """Query path table for files matching the pattern.

    @brief List documented files matching a glob-like pattern.
    @version 1
    """
    conn = _get_db()
    sql_pattern = pattern.replace("*", "%")
    files = conn.execute(
        """
        SELECT p.name, c.briefdescription
        FROM path p
        LEFT JOIN compounddef c ON c.file_id = p.rowid AND c.kind = 'file'
        WHERE p.type = 1 AND p.name LIKE ?
        ORDER BY p.name
        """,
        (sql_pattern,),
    ).fetchall()
    conn.close()

    if not files:
        return f"No files found matching '{pattern}'."

    results: list[str] = []
    for f in files:
        brief = _clean_doxy(f["briefdescription"]) if f["briefdescription"] else ""
        line = f"- `{f['name']}`"
        if brief:
            line += f" — {brief}"
        results.append(line)

    return "\n".join(results)


## @brief Get inheritance hierarchy tree for a class.
## @param class_name Class name to look up.
## @return Formatted text with hierarchy tree.
## @utility
## @version 1
def _get_hierarchy(class_name: str) -> str:
    """Build and format the inheritance tree for a class.

    @brief Get inheritance hierarchy tree for a class.
    @version 1
    """
    conn = _get_db()
    target = conn.execute(
        """
        SELECT c.rowid, c.name FROM compounddef c
        WHERE c.kind IN ('class', 'struct') AND c.name LIKE ?
        LIMIT 1
        """,
        (f"%{class_name}%",),
    ).fetchone()

    if not target:
        conn.close()
        return f"No class found matching '{class_name}'."

    lines: list[str] = [f"## Hierarchy for {target['name']}"]
    ancestors = _walk_ancestors(conn, target["rowid"])
    descendants = _walk_descendants(conn, target["rowid"])

    if ancestors:
        lines.append("\n**Ancestors:**")
        for depth, name in ancestors:
            lines.append(f"{'  ' * depth}← {name}")

    lines.append(f"\n**→ {target['name']}**")

    if descendants:
        lines.append("\n**Descendants:**")
        for depth, name in descendants:
            lines.append(f"{'  ' * depth}→ {name}")

    conn.close()
    return "\n".join(lines)


## @brief Walk up the inheritance chain collecting ancestors.
## @param conn SQLite connection.
## @param rowid Starting compound rowid.
## @return List of (depth, name) tuples.
## @utility
## @version 1
def _walk_ancestors(
    conn: sqlite3.Connection,
    rowid: int,
) -> list[tuple[int, str]]:
    """Recursively collect base classes.

    @brief Walk up the inheritance chain collecting ancestors.
    @version 1
    """
    result: list[tuple[int, str]] = []
    _collect_ancestors(conn, rowid, 0, result, set())
    return result


## @brief Recursive helper for ancestor collection.
## @param conn SQLite connection.
## @param rowid Current compound rowid.
## @param depth Current recursion depth.
## @param result Accumulator list.
## @param visited Set of visited rowids to prevent cycles.
## @utility
## @version 1
def _collect_ancestors(
    conn: sqlite3.Connection,
    rowid: int,
    depth: int,
    result: list[tuple[int, str]],
    visited: set[int],
) -> None:
    """Recursive ancestor collection with cycle prevention.

    @brief Recursive helper for ancestor collection.
    @version 1
    """
    bases = conn.execute(
        """
        SELECT cr.base_rowid, c.name FROM compoundref cr
        JOIN compounddef c ON cr.base_rowid = c.rowid
        WHERE cr.derived_rowid = ?
        """,
        (rowid,),
    ).fetchall()
    for base in bases:
        if base["base_rowid"] in visited:
            continue
        visited.add(base["base_rowid"])
        _collect_ancestors(conn, base["base_rowid"], depth + 1, result, visited)
        result.append((depth, base["name"]))


## @brief Walk down the inheritance chain collecting descendants.
## @param conn SQLite connection.
## @param rowid Starting compound rowid.
## @return List of (depth, name) tuples.
## @utility
## @version 1
def _walk_descendants(
    conn: sqlite3.Connection,
    rowid: int,
) -> list[tuple[int, str]]:
    """Recursively collect derived classes.

    @brief Walk down the inheritance chain collecting descendants.
    @version 1
    """
    result: list[tuple[int, str]] = []
    _collect_descendants(conn, rowid, 1, result, set())
    return result


## @brief Recursive helper for descendant collection.
## @param conn SQLite connection.
## @param rowid Current compound rowid.
## @param depth Current recursion depth.
## @param result Accumulator list.
## @param visited Set of visited rowids to prevent cycles.
## @utility
## @version 1
def _collect_descendants(
    conn: sqlite3.Connection,
    rowid: int,
    depth: int,
    result: list[tuple[int, str]],
    visited: set[int],
) -> None:
    """Recursive descendant collection with cycle prevention.

    @brief Recursive helper for descendant collection.
    @version 1
    """
    derived = conn.execute(
        """
        SELECT cr.derived_rowid, c.name FROM compoundref cr
        JOIN compounddef c ON cr.derived_rowid = c.rowid
        WHERE cr.base_rowid = ?
        """,
        (rowid,),
    ).fetchall()
    for d in derived:
        if d["derived_rowid"] in visited:
            continue
        visited.add(d["derived_rowid"])
        result.append((depth, d["name"]))
        _collect_descendants(conn, d["derived_rowid"], depth + 1, result, visited)


## @brief Strip doxygen XML noise from description text.
## @param text Raw doxygen description string.
## @return Cleaned plain text.
## @utility
## @version 1
def _clean_doxy(text: str) -> str:
    """Remove doxygen XML tags from description text.

    @brief Strip doxygen XML noise from description text.
    @version 1
    """
    import re

    cleaned = re.sub(r"<[^>]+>", "", text)
    cleaned = cleaned.replace("\n", " ").strip()
    return cleaned


## @brief Parse newline-delimited JSON-RPC request from stdin.
## @return Parsed dict or None on EOF.
## @utility
## @version 1
def _read_jsonrpc() -> dict[str, Any] | None:
    """Read one JSON-RPC 2.0 message from stdin.

    @brief Parse newline-delimited JSON-RPC request from stdin.
    @version 1
    """
    line = sys.stdin.readline()
    if not line:
        return None
    return json.loads(line.strip())


## @brief Send newline-delimited JSON-RPC response to stdout.
## @param response Response dict.
## @utility
## @version 1
def _write_jsonrpc(response: dict[str, Any]) -> None:
    """Write one JSON-RPC 2.0 message to stdout.

    @brief Send newline-delimited JSON-RPC response to stdout.
    @version 1
    """
    sys.stdout.write(json.dumps(response, separators=(",", ":")) + "\n")
    sys.stdout.flush()


## @brief Write JSON-RPC success response.
## @param rid Request ID.
## @param result Result payload dict.
## @utility
## @version 1
def _jsonrpc_ok(rid: int | None, result: dict[str, Any]) -> None:
    """Send a JSON-RPC success response.

    @brief Write JSON-RPC success response.
    @version 1
    """
    _write_jsonrpc({"jsonrpc": "2.0", "id": rid, "result": result})


## @brief Write JSON-RPC error response.
## @param rid Request ID.
## @param code Error code.
## @param message Error message.
## @utility
## @version 1
def _jsonrpc_err(rid: int | None, code: int, message: str) -> None:
    """Send a JSON-RPC error response.

    @brief Write JSON-RPC error response.
    @version 1
    """
    _write_jsonrpc({"jsonrpc": "2.0", "id": rid, "error": {"code": code, "message": message}})


## @brief Dispatch a tools/call request to the appropriate handler.
## @param rid Request ID.
## @param params Request parameters dict.
## @utility
## @version 1
def _handle_tools_call(rid: int | None, params: dict[str, Any]) -> None:
    """Route tool calls to implementation functions.

    @brief Dispatch a tools/call request to the appropriate handler.
    @version 1
    """
    name = params.get("name", "")
    args = params.get("arguments", {})

    handlers: dict[str, Any] = {
        "lookup_function": lambda: _lookup_function(args.get("name", "")),
        "lookup_class": lambda: _lookup_class(args.get("name", "")),
        "search": lambda: _search(args.get("query", ""), args.get("limit", 10)),
        "list_files": lambda: _list_files(args.get("pattern", "*")),
        "get_hierarchy": lambda: _get_hierarchy(args.get("class_name", "")),
    }

    handler = handlers.get(name)
    if handler:
        text = handler()
        _jsonrpc_ok(rid, {"content": [{"type": "text", "text": text}]})
    else:
        _jsonrpc_err(rid, -32601, f"Unknown tool: {name}")


## @brief Route JSON-RPC method to handler.
## @param request Parsed JSON-RPC request dict.
## @utility
## @version 1
def _dispatch(request: dict[str, Any]) -> None:
    """Dispatch a JSON-RPC request to the appropriate handler.

    @brief Route JSON-RPC method to handler.
    @version 1
    """
    method = request.get("method", "")
    params = request.get("params", {})
    rid = request.get("id")
    handlers: dict[str, Any] = {
        "initialize": lambda: _jsonrpc_ok(
            rid,
            {
                "protocolVersion": "2024-11-05",
                "serverInfo": {"name": "docs"},
                "capabilities": {},
            },
        ),
        "tools/list": lambda: _jsonrpc_ok(rid, {"tools": _MCP_TOOLS}),
        "tools/call": lambda: _handle_tools_call(rid, params),
    }
    handler = handlers.get(method)
    if handler:
        handler()
    else:
        _jsonrpc_err(rid, -32601, f"Unknown method: {method}")


## @brief Main MCP server loop over stdio.
## @utility
## @version 1
def serve_stdio() -> None:
    """Run the MCP server loop over stdio transport.

    @brief Main MCP server loop over stdio.
    @version 1
    """
    while True:
        request = _read_jsonrpc()
        if request is None:
            break
        _dispatch(request)


if __name__ == "__main__":
    serve_stdio()
