"""
Web MCP server.

Provides web search (DuckDuckGo) and URL fetching capabilities.
No API keys required.
"""

from __future__ import annotations

import json
import logging
import re
from html.parser import HTMLParser
from typing import Any
from urllib.parse import quote_plus

import httpx

from entropic.mcp.servers.base import BaseMCPServer
from entropic.mcp.tools import BaseTool

logger = logging.getLogger(__name__)

_USER_AGENT = "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0"
_TIMEOUT = 15.0


class _TextExtractor(HTMLParser):
    """Extract readable text from HTML, stripping tags and scripts."""

    def __init__(self) -> None:
        super().__init__()
        self._pieces: list[str] = []
        self._skip_depth = 0
        self._skip_tags = {"script", "style", "noscript", "svg", "head"}

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        if tag in self._skip_tags:
            self._skip_depth += 1
        if tag in ("br", "p", "div", "li", "tr", "h1", "h2", "h3", "h4", "h5", "h6"):
            self._pieces.append("\n")

    def handle_endtag(self, tag: str) -> None:
        if tag in self._skip_tags and self._skip_depth > 0:
            self._skip_depth -= 1

    def handle_data(self, data: str) -> None:
        if self._skip_depth == 0:
            self._pieces.append(data)

    def get_text(self) -> str:
        raw = "".join(self._pieces)
        # Collapse whitespace runs but preserve paragraph breaks
        raw = re.sub(r"[ \t]+", " ", raw)
        raw = re.sub(r"\n{3,}", "\n\n", raw)
        return raw.strip()


class _DuckDuckGoParser(HTMLParser):
    """Parse DuckDuckGo HTML search results."""

    def __init__(self) -> None:
        super().__init__()
        self.results: list[dict[str, str]] = []
        self._in_result_link = False
        self._in_snippet = False
        self._current: dict[str, str] = {}
        self._text_buf: list[str] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        attr_dict: dict[str, str] = {k: v or "" for k, v in attrs}
        cls = attr_dict.get("class", "")

        if tag == "a" and "result__a" in cls:
            self._in_result_link = True
            href = attr_dict.get("href", "")
            self._current = {"url": href, "title": "", "snippet": ""}
            self._text_buf = []

        if tag == "a" and "result__snippet" in cls:
            self._in_snippet = True
            self._text_buf = []

    def handle_endtag(self, tag: str) -> None:
        if tag == "a" and self._in_result_link:
            self._in_result_link = False
            self._current["title"] = " ".join(self._text_buf).strip()

        if tag == "a" and self._in_snippet:
            self._in_snippet = False
            self._current["snippet"] = " ".join(self._text_buf).strip()
            if self._current.get("url") and self._current.get("title"):
                self.results.append(self._current)
            self._current = {}

    def handle_data(self, data: str) -> None:
        if self._in_result_link or self._in_snippet:
            self._text_buf.append(data)


def _extract_text(html: str) -> str:
    """Extract readable text from HTML."""
    parser = _TextExtractor()
    parser.feed(html)
    return parser.get_text()


def _parse_ddg_results(html: str) -> list[dict[str, str]]:
    """Parse DuckDuckGo HTML results page."""
    parser = _DuckDuckGoParser()
    parser.feed(html)
    return parser.results


class WebSearchTool(BaseTool):
    """Search the web via DuckDuckGo HTML."""

    def __init__(self) -> None:
        super().__init__("web_search", "web")

    async def execute(self, arguments: dict[str, Any]) -> str:
        query = arguments.get("query", "")
        if not query:
            return json.dumps({"error": "missing_param", "message": "query is required"})

        max_results = min(arguments.get("max_results", 5), 10)

        try:
            results = await _ddg_search(query, max_results)
        except Exception as e:
            logger.error("Web search failed: %s", e)
            return json.dumps({"error": "search_failed", "message": str(e)})

        return json.dumps({"query": query, "results": results})


class WebFetchTool(BaseTool):
    """Fetch a URL and return text content."""

    def __init__(self) -> None:
        super().__init__("web_fetch", "web")

    async def execute(self, arguments: dict[str, Any]) -> str:
        url = arguments.get("url", "")
        if not url:
            return json.dumps({"error": "missing_param", "message": "url is required"})

        max_length = min(arguments.get("max_length", 20000), 50000)

        try:
            text = await _fetch_url(url, max_length)
        except Exception as e:
            logger.error("Web fetch failed for %s: %s", url, e)
            return json.dumps({"error": "fetch_failed", "message": str(e)})

        return json.dumps({"url": url, "length": len(text), "content": text})


async def _ddg_search(query: str, max_results: int) -> list[dict[str, str]]:
    """Search DuckDuckGo HTML and parse results."""
    url = f"https://html.duckduckgo.com/html/?q={quote_plus(query)}"
    headers = {"User-Agent": _USER_AGENT}

    async with httpx.AsyncClient(timeout=_TIMEOUT, follow_redirects=True) as client:
        resp = await client.get(url, headers=headers)
        resp.raise_for_status()

    results = _parse_ddg_results(resp.text)

    # Clean up DDG redirect URLs
    for r in results:
        if r["url"].startswith("//duckduckgo.com/l/?"):
            # Extract actual URL from DDG redirect
            match = re.search(r"uddg=([^&]+)", r["url"])
            if match:
                from urllib.parse import unquote

                r["url"] = unquote(match.group(1))

    return results[:max_results]


async def _fetch_url(url: str, max_length: int) -> str:
    """Fetch URL and return extracted text."""
    headers = {"User-Agent": _USER_AGENT}

    async with httpx.AsyncClient(timeout=_TIMEOUT, follow_redirects=True) as client:
        resp = await client.get(url, headers=headers)
        resp.raise_for_status()

    content_type = resp.headers.get("content-type", "")

    if "text/html" in content_type:
        text = _extract_text(resp.text)
    else:
        # Plain text, JSON, etc — return as-is
        text = resp.text

    if len(text) > max_length:
        text = text[:max_length] + f"\n\n[Truncated at {max_length} characters]"

    return text


class WebServer(BaseMCPServer):
    """Web search and fetch MCP server."""

    def __init__(self) -> None:
        super().__init__("web")
        self.register_tool(WebSearchTool())
        self.register_tool(WebFetchTool())
