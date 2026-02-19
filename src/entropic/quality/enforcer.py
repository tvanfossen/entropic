"""
Code quality enforcement coordinator.

Orchestrates quality analyzers and manages regeneration loop.
"""

import re
from collections.abc import Callable, Coroutine
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from entropic.config.schema import QualityConfig, QualityRulesConfig
from entropic.core.logging import get_logger
from entropic.quality.analyzers.base import AnalysisResult, CodeAnalyzer

logger = get_logger("quality.enforcer")


@dataclass
class QualityReport:
    """Aggregated quality report."""

    passed: bool
    violations: list[dict[str, Any]] = field(default_factory=list)
    warnings: list[dict[str, Any]] = field(default_factory=list)
    metrics: dict[str, Any] = field(default_factory=dict)

    def add_result(self, result: AnalysisResult) -> None:
        """Add analyzer result to report."""
        self.violations.extend(result.violations)
        self.warnings.extend(result.warnings)
        self.metrics.update(result.metrics)

        if result.violations:
            self.passed = False

    def format_feedback(self) -> str:
        """Format feedback for model regeneration."""
        if self.passed:
            return ""

        lines = ["The generated code has the following issues:\n"]

        for i, v in enumerate(self.violations, 1):
            location = f"line {v['line']}" if v.get("line") else ""
            lines.append(f"{i}. [{v['rule']}] {v['message']} {location}")

        lines.append("\nPlease fix these issues and regenerate the code.")
        return "\n".join(lines)


class QualityEnforcer:
    """
    Coordinates code quality enforcement.

    Runs analyzers, aggregates results, and manages
    regeneration loop for quality failures.
    """

    # Language detection by file extension
    LANGUAGE_MAP = {
        ".py": "python",
        ".js": "javascript",
        ".ts": "typescript",
        ".jsx": "javascript",
        ".tsx": "typescript",
        ".rs": "rust",
        ".go": "go",
        ".java": "java",
        ".rb": "ruby",
        ".php": "php",
        ".cs": "csharp",
        ".cpp": "cpp",
        ".c": "c",
        ".h": "c",
        ".hpp": "cpp",
    }

    def __init__(self, config: QualityConfig) -> None:
        """
        Initialize enforcer.

        Args:
            config: Quality configuration
        """
        self.config = config
        self._analyzers: dict[str, list[CodeAnalyzer]] = {}
        self._setup_analyzers()

    def _setup_analyzers(self) -> None:
        """Set up analyzers for each language."""
        from entropic.quality.analyzers.complexity import CognitiveComplexityAnalyzer
        from entropic.quality.analyzers.docstrings import DocstringAnalyzer
        from entropic.quality.analyzers.structure import StructureAnalyzer
        from entropic.quality.analyzers.typing import TypeHintAnalyzer

        # Python analyzers
        self._analyzers["python"] = [
            CognitiveComplexityAnalyzer(),
            TypeHintAnalyzer(),
            DocstringAnalyzer(),
            StructureAnalyzer(),
        ]

        # JavaScript/TypeScript - limited analysis
        self._analyzers["javascript"] = [
            CognitiveComplexityAnalyzer(),
            StructureAnalyzer(),
        ]
        self._analyzers["typescript"] = self._analyzers["javascript"]

    def analyze(
        self,
        code: str,
        language: str | None = None,
        filename: str | None = None,
    ) -> QualityReport:
        """
        Analyze code for quality issues.

        Args:
            code: Code to analyze
            language: Language (auto-detected if None)
            filename: Optional filename for context

        Returns:
            Quality report
        """
        if not self.config.enabled:
            return QualityReport(passed=True)

        # Detect language
        if language is None and filename:
            ext = Path(filename).suffix.lower()
            language = self.LANGUAGE_MAP.get(ext, "python")
        language = language or "python"

        # Get rules for language
        rules = self._get_rules(language)

        # Run analyzers
        report = QualityReport(passed=True)
        analyzers = self._analyzers.get(language, [])

        for analyzer in analyzers:
            try:
                result = analyzer.analyze(code, rules)
                report.add_result(result)
            except Exception as e:
                logger.warning(f"Analyzer {analyzer.__class__.__name__} failed: {e}")

        return report

    def _get_rules(self, language: str) -> QualityRulesConfig:
        """Get rules for a specific language."""
        # Check for language override
        if language in self.config.language_overrides:
            return self.config.language_overrides[language]
        return self.config.rules

    def extract_code_blocks(self, content: str) -> list[dict[str, str]]:
        """
        Extract code blocks from markdown-style response.

        Args:
            content: Response content

        Returns:
            List of {language, code, filename} dicts
        """
        blocks = []

        # Match ```language\ncode``` blocks
        pattern = r"```(\w+)?\n(.*?)```"
        matches = re.findall(pattern, content, re.DOTALL)

        for language, code in matches:
            blocks.append(
                {
                    "language": language or "python",
                    "code": code.strip(),
                    "filename": None,
                }
            )

        return blocks

    async def enforce(
        self,
        content: str,
        regenerate_fn: Callable[[str], Coroutine[Any, Any, str]],
        max_attempts: int | None = None,
    ) -> tuple[str, QualityReport]:
        """
        Enforce quality with optional regeneration.

        Args:
            content: Model response content
            regenerate_fn: Async function to regenerate with feedback
            max_attempts: Max regeneration attempts

        Returns:
            Tuple of (final content, final report)
        """
        max_attempts = max_attempts or self.config.max_regeneration_attempts

        for attempt in range(max_attempts):
            # Extract and analyze code blocks
            blocks = self.extract_code_blocks(content)

            if not blocks:
                return content, QualityReport(passed=True)

            # Analyze all blocks
            combined_report = QualityReport(passed=True)
            for block in blocks:
                report = self.analyze(
                    block["code"],
                    block["language"],
                    block.get("filename"),
                )
                combined_report.violations.extend(report.violations)
                combined_report.warnings.extend(report.warnings)
                combined_report.metrics.update(report.metrics)
                if not report.passed:
                    combined_report.passed = False

            if combined_report.passed:
                logger.debug(f"Quality check passed on attempt {attempt + 1}")
                return content, combined_report

            # Generate feedback and retry
            if attempt < max_attempts - 1:
                feedback = combined_report.format_feedback()
                logger.info(f"Quality check failed, regenerating (attempt {attempt + 2})")
                content = await regenerate_fn(feedback)

        return content, combined_report
