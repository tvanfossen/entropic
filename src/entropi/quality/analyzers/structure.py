"""
Structure analyzer.

Checks function size, return count, and naming conventions.
"""

import ast
import re

from entropi.config.schema import QualityRulesConfig
from entropi.quality.analyzers.base import AnalysisResult, CodeAnalyzer


class StructureAnalyzer(CodeAnalyzer):
    """Analyzes code structure and conventions."""

    @property
    def name(self) -> str:
        return "structure"

    def analyze(self, code: str, rules: QualityRulesConfig) -> AnalysisResult:
        """Analyze code structure."""
        result = AnalysisResult()

        try:
            tree = ast.parse(code)
        except SyntaxError:
            return result

        lines = code.split("\n")
        result.metrics["total_lines"] = len(lines)

        # Check file length
        if len(lines) > rules.max_file_lines:
            result.add_warning(
                "file_too_long",
                f"File has {len(lines)} lines (max: {rules.max_file_lines})",
            )

        for node in ast.walk(tree):
            if isinstance(node, ast.FunctionDef | ast.AsyncFunctionDef):
                self._check_function(node, rules, result)
            elif isinstance(node, ast.ClassDef):
                self._check_class(node, rules, result)

        return result

    def _check_function(
        self,
        node: ast.FunctionDef | ast.AsyncFunctionDef,
        rules: QualityRulesConfig,
        result: AnalysisResult,
    ) -> None:
        """Check function structure."""
        # Check function length
        func_lines = node.end_lineno - node.lineno + 1 if node.end_lineno else 0
        if func_lines > rules.max_function_lines:
            result.add_violation(
                "function_too_long",
                f"Function '{node.name}' has {func_lines} lines "
                f"(max: {rules.max_function_lines})",
                line=node.lineno,
            )

        # Check parameter count
        param_count = len([arg for arg in node.args.args if arg.arg not in ("self", "cls")])
        if param_count > rules.max_parameters:
            result.add_violation(
                "too_many_parameters",
                f"Function '{node.name}' has {param_count} parameters "
                f"(max: {rules.max_parameters})",
                line=node.lineno,
            )

        # Count returns
        return_count = self._count_returns(node)
        if return_count > rules.max_returns_per_function:
            result.add_violation(
                "too_many_returns",
                f"Function '{node.name}' has {return_count} return statements "
                f"(max: {rules.max_returns_per_function})",
                line=node.lineno,
            )

        # Check naming convention
        if rules.enforce_snake_case_functions:
            if not self._is_snake_case(node.name) and not node.name.startswith("__"):
                result.add_warning(
                    "naming_convention",
                    f"Function '{node.name}' should use snake_case",
                    line=node.lineno,
                )

    def _check_class(
        self,
        node: ast.ClassDef,
        rules: QualityRulesConfig,
        result: AnalysisResult,
    ) -> None:
        """Check class structure."""
        if rules.enforce_pascal_case_classes:
            if not self._is_pascal_case(node.name):
                result.add_warning(
                    "naming_convention",
                    f"Class '{node.name}' should use PascalCase",
                    line=node.lineno,
                )

    def _count_returns(self, node: ast.AST) -> int:
        """Count return statements in a function."""
        count = 0
        for child in ast.walk(node):
            if isinstance(child, ast.Return):
                count += 1
        return count

    def _is_snake_case(self, name: str) -> bool:
        """Check if name is snake_case."""
        return bool(re.match(r"^[a-z][a-z0-9_]*$", name))

    def _is_pascal_case(self, name: str) -> bool:
        """Check if name is PascalCase."""
        return bool(re.match(r"^[A-Z][a-zA-Z0-9]*$", name))
