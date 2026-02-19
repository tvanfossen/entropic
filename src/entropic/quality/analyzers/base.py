"""
Base analyzer interface.

All quality analyzers inherit from this base class.
"""

from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import Any

from entropic.config.schema import QualityRulesConfig


@dataclass
class Violation:
    """A quality violation."""

    rule: str
    message: str
    line: int | None = None
    column: int | None = None
    severity: str = "error"  # error, warning, info


@dataclass
class AnalysisResult:
    """Result of running an analyzer."""

    violations: list[dict[str, Any]] = field(default_factory=list)
    warnings: list[dict[str, Any]] = field(default_factory=list)
    metrics: dict[str, Any] = field(default_factory=dict)

    def add_violation(
        self,
        rule: str,
        message: str,
        line: int | None = None,
        severity: str = "error",
    ) -> None:
        """Add a violation."""
        self.violations.append(
            {
                "rule": rule,
                "message": message,
                "line": line,
                "severity": severity,
            }
        )

    def add_warning(
        self,
        rule: str,
        message: str,
        line: int | None = None,
    ) -> None:
        """Add a warning."""
        self.warnings.append(
            {
                "rule": rule,
                "message": message,
                "line": line,
                "severity": "warning",
            }
        )


class CodeAnalyzer(ABC):
    """Base class for code analyzers."""

    @property
    @abstractmethod
    def name(self) -> str:
        """Analyzer name."""
        pass

    @abstractmethod
    def analyze(self, code: str, rules: QualityRulesConfig) -> AnalysisResult:
        """
        Analyze code for quality issues.

        Args:
            code: Code to analyze
            rules: Quality rules configuration

        Returns:
            Analysis result
        """
        pass
