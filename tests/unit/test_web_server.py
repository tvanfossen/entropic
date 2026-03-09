"""Tests for WebServer — web search and fetch tools."""

import json
from unittest.mock import AsyncMock, patch

import pytest
from entropic.mcp.servers.web import (
    WebServer,
    _extract_text,
    _parse_ddg_results,
)


class TestTextExtractor:
    """HTML to text extraction."""

    def test_strips_tags(self) -> None:
        html = "<p>Hello <b>world</b></p>"
        assert "Hello world" in _extract_text(html)

    def test_strips_scripts(self) -> None:
        html = "<p>Before</p><script>alert('x')</script><p>After</p>"
        text = _extract_text(html)
        assert "Before" in text
        assert "After" in text
        assert "alert" not in text

    def test_strips_style(self) -> None:
        html = "<style>body { color: red; }</style><p>Content</p>"
        text = _extract_text(html)
        assert "Content" in text
        assert "color" not in text

    def test_preserves_line_breaks(self) -> None:
        html = "<p>First</p><p>Second</p>"
        text = _extract_text(html)
        assert "First" in text
        assert "Second" in text


class TestDDGParser:
    """DuckDuckGo HTML result parsing."""

    def test_parses_result(self) -> None:
        html = """
        <a class="result__a" href="https://example.com">Example Title</a>
        <a class="result__snippet" href="#">This is a snippet</a>
        """
        results = _parse_ddg_results(html)
        assert len(results) == 1
        assert results[0]["title"] == "Example Title"
        assert results[0]["url"] == "https://example.com"
        assert results[0]["snippet"] == "This is a snippet"

    def test_empty_html(self) -> None:
        assert _parse_ddg_results("<html></html>") == []


class TestWebSearchTool:
    """web_search tool execution."""

    @pytest.fixture
    def server(self) -> WebServer:
        return WebServer()

    @pytest.mark.asyncio
    async def test_missing_query(self, server: WebServer) -> None:
        result = await server.execute_tool("web_search", {})
        data = json.loads(result)
        assert data["error"] == "missing_param"

    @pytest.mark.asyncio
    async def test_search_returns_results(self, server: WebServer) -> None:
        fake_html = """
        <a class="result__a" href="https://example.com">Example</a>
        <a class="result__snippet" href="#">A snippet</a>
        """
        mock_response = AsyncMock()
        mock_response.text = fake_html
        mock_response.raise_for_status = lambda: None

        with patch("entropic.mcp.servers.web.httpx.AsyncClient") as mock_client:
            instance = AsyncMock()
            instance.get.return_value = mock_response
            instance.__aenter__ = AsyncMock(return_value=instance)
            instance.__aexit__ = AsyncMock(return_value=False)
            mock_client.return_value = instance

            result = await server.execute_tool("web_search", {"query": "test"})
            data = json.loads(result)
            assert "results" in data
            assert len(data["results"]) == 1
            assert data["results"][0]["title"] == "Example"


class TestWebFetchTool:
    """web_fetch tool execution."""

    @pytest.fixture
    def server(self) -> WebServer:
        return WebServer()

    @pytest.mark.asyncio
    async def test_missing_url(self, server: WebServer) -> None:
        result = await server.execute_tool("web_fetch", {})
        data = json.loads(result)
        assert data["error"] == "missing_param"

    @pytest.mark.asyncio
    async def test_fetch_returns_content(self, server: WebServer) -> None:
        mock_response = AsyncMock()
        mock_response.text = "<html><body><p>Hello World</p></body></html>"
        mock_response.headers = {"content-type": "text/html"}
        mock_response.raise_for_status = lambda: None

        with patch("entropic.mcp.servers.web.httpx.AsyncClient") as mock_client:
            instance = AsyncMock()
            instance.get.return_value = mock_response
            instance.__aenter__ = AsyncMock(return_value=instance)
            instance.__aexit__ = AsyncMock(return_value=False)
            mock_client.return_value = instance

            result = await server.execute_tool("web_fetch", {"url": "https://example.com"})
            data = json.loads(result)
            assert "content" in data
            assert "Hello World" in data["content"]

    @pytest.mark.asyncio
    async def test_truncates_long_content(self, server: WebServer) -> None:
        long_text = "x" * 50000
        mock_response = AsyncMock()
        mock_response.text = long_text
        mock_response.headers = {"content-type": "text/plain"}
        mock_response.raise_for_status = lambda: None

        with patch("entropic.mcp.servers.web.httpx.AsyncClient") as mock_client:
            instance = AsyncMock()
            instance.get.return_value = mock_response
            instance.__aenter__ = AsyncMock(return_value=instance)
            instance.__aexit__ = AsyncMock(return_value=False)
            mock_client.return_value = instance

            result = await server.execute_tool(
                "web_fetch", {"url": "https://example.com", "max_length": 100}
            )
            data = json.loads(result)
            assert "Truncated" in data["content"]
            assert data["length"] > 100


class TestWebServerTools:
    """Server tool registration."""

    def test_has_both_tools(self) -> None:
        server = WebServer()
        tool_names = [t.name for t in server.get_tools()]
        assert "web_search" in tool_names
        assert "web_fetch" in tool_names
