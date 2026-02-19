"""
Cognitive complexity analyzer.

Implements SonarSource cognitive complexity calculation.
"""

import ast

from entropic.config.schema import QualityRulesConfig
from entropic.quality.analyzers.base import AnalysisResult, CodeAnalyzer


class CognitiveComplexityAnalyzer(CodeAnalyzer):
    """
    Analyzes cognitive complexity of Python code.

    Based on SonarSource cognitive complexity specification.
    """

    @property
    def name(self) -> str:
        return "cognitive_complexity"

    def analyze(self, code: str, rules: QualityRulesConfig) -> AnalysisResult:
        """Analyze code for cognitive complexity."""
        result = AnalysisResult()

        try:
            tree = ast.parse(code)
        except SyntaxError as e:
            result.add_violation(
                "syntax_error",
                f"Invalid Python syntax: {e}",
                line=e.lineno,
            )
            return result

        # Analyze each function
        for node in ast.walk(tree):
            if isinstance(node, ast.FunctionDef | ast.AsyncFunctionDef):
                complexity = self._calculate_complexity(node)

                result.metrics[f"{node.name}_complexity"] = complexity

                if complexity > rules.max_cognitive_complexity:
                    result.add_violation(
                        "cognitive_complexity",
                        f"Function '{node.name}' has cognitive complexity {complexity} "
                        f"(max: {rules.max_cognitive_complexity})",
                        line=node.lineno,
                    )

                # Also check cyclomatic complexity
                cyclomatic = self._calculate_cyclomatic(node)
                if cyclomatic > rules.max_cyclomatic_complexity:
                    result.add_violation(
                        "cyclomatic_complexity",
                        f"Function '{node.name}' has cyclomatic complexity {cyclomatic} "
                        f"(max: {rules.max_cyclomatic_complexity})",
                        line=node.lineno,
                    )

        return result

    def _calculate_complexity(self, node: ast.AST, nesting: int = 0) -> int:
        """
        Calculate cognitive complexity.

        Increments:
        - +1 for each if, elif, else, for, while, except, with
        - +1 for each and/or in boolean expression
        - +nesting for nested structures
        - +1 for recursion
        """
        complexity = 0

        for child in ast.walk(node):
            if child is node:
                continue

            # Control flow structures
            if isinstance(child, ast.If | ast.For | ast.While | ast.ExceptHandler | ast.With):
                complexity += 1 + self._get_nesting_level(child, node)

            # Boolean operators
            elif isinstance(child, ast.BoolOp):
                complexity += len(child.values) - 1

            # Ternary expressions
            elif isinstance(child, ast.IfExp):
                complexity += 1

            # Comprehensions add complexity
            elif isinstance(child, ast.ListComp | ast.DictComp | ast.SetComp | ast.GeneratorExp):
                complexity += 1 + len(child.generators) - 1

        return complexity

    def _calculate_cyclomatic(self, node: ast.AST) -> int:
        """Calculate cyclomatic complexity."""
        complexity = 1  # Base complexity

        for child in ast.walk(node):
            if isinstance(child, ast.If | ast.While | ast.For | ast.ExceptHandler):
                complexity += 1
            elif isinstance(child, ast.BoolOp):
                complexity += len(child.values) - 1

        return complexity

    def _get_nesting_level(self, node: ast.AST, root: ast.AST) -> int:
        """Get nesting level of a node within root."""
        level = 0

        # Walk up to find parent nesting structures
        for potential_parent in ast.walk(root):
            if self._is_parent_of(potential_parent, node):
                if isinstance(
                    potential_parent,
                    ast.If | ast.For | ast.While | ast.With | ast.ExceptHandler,
                ):
                    level += 1

        return level

    def _is_parent_of(self, parent: ast.AST, child: ast.AST) -> bool:
        """Check if parent contains child."""
        for node in ast.walk(parent):
            if node is child:
                return True
        return False
