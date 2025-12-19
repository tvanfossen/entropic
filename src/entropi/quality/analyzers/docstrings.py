"""
Docstring analyzer.

Checks for presence and format of docstrings.
"""

import ast

from entropi.config.schema import QualityRulesConfig
from entropi.quality.analyzers.base import AnalysisResult, CodeAnalyzer


class DocstringAnalyzer(CodeAnalyzer):
    """Analyzes docstring presence and format."""

    @property
    def name(self) -> str:
        return "docstrings"

    def analyze(self, code: str, rules: QualityRulesConfig) -> AnalysisResult:
        """Analyze code for docstring coverage."""
        result = AnalysisResult()

        if not rules.require_docstrings:
            return result

        try:
            tree = ast.parse(code)
        except SyntaxError:
            return result

        for node in ast.walk(tree):
            if isinstance(node, ast.FunctionDef | ast.AsyncFunctionDef):
                self._check_function_docstring(node, rules, result)
            elif isinstance(node, ast.ClassDef):
                self._check_class_docstring(node, rules, result)

        return result

    def _check_function_docstring(
        self,
        node: ast.FunctionDef | ast.AsyncFunctionDef,
        rules: QualityRulesConfig,
        result: AnalysisResult,
    ) -> None:
        """Check function docstring."""
        # Skip private methods
        if node.name.startswith("_") and not node.name.startswith("__"):
            return

        docstring = ast.get_docstring(node)

        if docstring is None:
            result.add_violation(
                "missing_docstring",
                f"Function '{node.name}' has no docstring",
                line=node.lineno,
            )
            return

        # Check format if specified
        if rules.docstring_style == "google":
            self._check_google_style(node, docstring, result)
        elif rules.docstring_style == "numpy":
            self._check_numpy_style(node, docstring, result)

    def _check_class_docstring(
        self,
        node: ast.ClassDef,
        rules: QualityRulesConfig,
        result: AnalysisResult,
    ) -> None:
        """Check class docstring."""
        docstring = ast.get_docstring(node)

        if docstring is None:
            result.add_violation(
                "missing_docstring",
                f"Class '{node.name}' has no docstring",
                line=node.lineno,
            )

    def _check_google_style(
        self,
        node: ast.FunctionDef | ast.AsyncFunctionDef,
        docstring: str,
        result: AnalysisResult,
    ) -> None:
        """Check Google-style docstring format."""
        # Get parameters (excluding self/cls)
        params = [arg.arg for arg in node.args.args if arg.arg not in ("self", "cls")]

        # Check Args section if function has parameters
        if params and "Args:" not in docstring:
            result.add_warning(
                "docstring_format",
                f"Function '{node.name}' docstring missing 'Args:' section",
                line=node.lineno,
            )

        # Check Returns section if function has return annotation
        if node.returns and "Returns:" not in docstring:
            result.add_warning(
                "docstring_format",
                f"Function '{node.name}' docstring missing 'Returns:' section",
                line=node.lineno,
            )

    def _check_numpy_style(
        self,
        node: ast.FunctionDef | ast.AsyncFunctionDef,
        docstring: str,
        result: AnalysisResult,
    ) -> None:
        """Check NumPy-style docstring format."""
        params = [arg.arg for arg in node.args.args if arg.arg not in ("self", "cls")]

        if params and "Parameters\n----------" not in docstring:
            result.add_warning(
                "docstring_format",
                f"Function '{node.name}' docstring missing 'Parameters' section",
                line=node.lineno,
            )
