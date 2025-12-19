# Implementation 09: Quality Enforcement

> Code quality checks at generation time with multi-language support

**Prerequisites:** Implementation 08 complete
**Estimated Time:** 2-3 hours with Claude Code
**Checkpoint:** Generated code is validated against quality rules

---

## Objectives

1. Implement code quality analyzer framework
2. Create cognitive complexity analyzer
3. Build type hint and docstring validators
4. Implement regeneration loop for quality failures
5. Support per-language configuration

---

## 1. Quality Enforcer Framework

### File: `src/entropi/quality/enforcer.py`

```python
"""
Code quality enforcement coordinator.

Orchestrates quality analyzers and manages regeneration loop.
"""
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from entropi.config.schema import QualityConfig, QualityRulesConfig
from entropi.core.logging import get_logger
from entropi.quality.analyzers.base import AnalysisResult, CodeAnalyzer

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
            location = f"line {v['line']}" if v.get('line') else ""
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
        from entropi.quality.analyzers.complexity import CognitiveComplexityAnalyzer
        from entropi.quality.analyzers.typing import TypeHintAnalyzer
        from entropi.quality.analyzers.docstrings import DocstringAnalyzer
        from entropi.quality.analyzers.structure import StructureAnalyzer

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
        import re

        blocks = []

        # Match ```language\ncode``` blocks
        pattern = r"```(\w+)?\n(.*?)```"
        matches = re.findall(pattern, content, re.DOTALL)

        for language, code in matches:
            blocks.append({
                "language": language or "python",
                "code": code.strip(),
                "filename": None,
            })

        return blocks

    async def enforce(
        self,
        content: str,
        regenerate_fn: callable,
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
```

---

## 2. Base Analyzer

### File: `src/entropi/quality/analyzers/base.py`

```python
"""
Base analyzer interface.

All quality analyzers inherit from this base class.
"""
from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import Any

from entropi.config.schema import QualityRulesConfig


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
        self.violations.append({
            "rule": rule,
            "message": message,
            "line": line,
            "severity": severity,
        })

    def add_warning(
        self,
        rule: str,
        message: str,
        line: int | None = None,
    ) -> None:
        """Add a warning."""
        self.warnings.append({
            "rule": rule,
            "message": message,
            "line": line,
            "severity": "warning",
        })


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
```

---

## 3. Cognitive Complexity Analyzer

### File: `src/entropi/quality/analyzers/complexity.py`

```python
"""
Cognitive complexity analyzer.

Implements SonarSource cognitive complexity calculation.
"""
import ast
from typing import Any

from entropi.config.schema import QualityRulesConfig
from entropi.quality.analyzers.base import AnalysisResult, CodeAnalyzer


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
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
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
            if isinstance(child, (ast.If, ast.For, ast.While, ast.ExceptHandler, ast.With)):
                complexity += 1 + self._get_nesting_level(child, node)

            # Boolean operators
            elif isinstance(child, ast.BoolOp):
                complexity += len(child.values) - 1

            # Ternary expressions
            elif isinstance(child, ast.IfExp):
                complexity += 1

            # Comprehensions add complexity
            elif isinstance(child, (ast.ListComp, ast.DictComp, ast.SetComp, ast.GeneratorExp)):
                complexity += 1 + len(child.generators) - 1

        return complexity

    def _calculate_cyclomatic(self, node: ast.AST) -> int:
        """Calculate cyclomatic complexity."""
        complexity = 1  # Base complexity

        for child in ast.walk(node):
            if isinstance(child, (ast.If, ast.While, ast.For, ast.ExceptHandler)):
                complexity += 1
            elif isinstance(child, ast.BoolOp):
                complexity += len(child.values) - 1

        return complexity

    def _get_nesting_level(self, node: ast.AST, root: ast.AST) -> int:
        """Get nesting level of a node within root."""
        level = 0
        current = node

        # Walk up to find parent nesting structures
        for potential_parent in ast.walk(root):
            if self._is_parent_of(potential_parent, node):
                if isinstance(potential_parent, (ast.If, ast.For, ast.While, ast.With, ast.ExceptHandler)):
                    level += 1

        return level

    def _is_parent_of(self, parent: ast.AST, child: ast.AST) -> bool:
        """Check if parent contains child."""
        for node in ast.walk(parent):
            if node is child:
                return True
        return False
```

---

## 4. Type Hint Analyzer

### File: `src/entropi/quality/analyzers/typing.py`

```python
"""
Type hint analyzer.

Checks for presence and correctness of type hints.
"""
import ast

from entropi.config.schema import QualityRulesConfig
from entropi.quality.analyzers.base import AnalysisResult, CodeAnalyzer


class TypeHintAnalyzer(CodeAnalyzer):
    """Analyzes type hint coverage in Python code."""

    @property
    def name(self) -> str:
        return "type_hints"

    def analyze(self, code: str, rules: QualityRulesConfig) -> AnalysisResult:
        """Analyze code for type hint coverage."""
        result = AnalysisResult()

        if not rules.require_type_hints:
            return result

        try:
            tree = ast.parse(code)
        except SyntaxError:
            return result  # Syntax errors handled elsewhere

        for node in ast.walk(tree):
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                self._check_function(node, rules, result)

        return result

    def _check_function(
        self,
        node: ast.FunctionDef | ast.AsyncFunctionDef,
        rules: QualityRulesConfig,
        result: AnalysisResult,
    ) -> None:
        """Check a function for type hints."""
        # Skip dunder methods
        if node.name.startswith("__") and node.name.endswith("__"):
            return

        # Check parameters
        for arg in node.args.args:
            # Skip self/cls
            if arg.arg in ("self", "cls"):
                continue

            if arg.annotation is None:
                result.add_violation(
                    "missing_type_hint",
                    f"Parameter '{arg.arg}' in function '{node.name}' has no type hint",
                    line=node.lineno,
                )

        # Check return type
        if rules.require_return_type and node.returns is None:
            # Skip __init__
            if node.name != "__init__":
                result.add_violation(
                    "missing_return_type",
                    f"Function '{node.name}' has no return type hint",
                    line=node.lineno,
                )
```

---

## 5. Docstring Analyzer

### File: `src/entropi/quality/analyzers/docstrings.py`

```python
"""
Docstring analyzer.

Checks for presence and format of docstrings.
"""
import ast
import re

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
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
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
        params = [
            arg.arg for arg in node.args.args
            if arg.arg not in ("self", "cls")
        ]

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
        params = [
            arg.arg for arg in node.args.args
            if arg.arg not in ("self", "cls")
        ]

        if params and "Parameters\n----------" not in docstring:
            result.add_warning(
                "docstring_format",
                f"Function '{node.name}' docstring missing 'Parameters' section",
                line=node.lineno,
            )
```

---

## 6. Structure Analyzer

### File: `src/entropi/quality/analyzers/structure.py`

```python
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
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
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
                f"Function '{node.name}' has {func_lines} lines (max: {rules.max_function_lines})",
                line=node.lineno,
            )

        # Check parameter count
        param_count = len([
            arg for arg in node.args.args
            if arg.arg not in ("self", "cls")
        ])
        if param_count > rules.max_parameters:
            result.add_violation(
                "too_many_parameters",
                f"Function '{node.name}' has {param_count} parameters (max: {rules.max_parameters})",
                line=node.lineno,
            )

        # Count returns
        return_count = self._count_returns(node)
        if return_count > rules.max_returns_per_function:
            result.add_violation(
                "too_many_returns",
                f"Function '{node.name}' has {return_count} return statements (max: {rules.max_returns_per_function})",
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
```

---

## 7. Tests

### File: `tests/unit/test_quality.py`

```python
"""Tests for quality enforcement."""
import pytest

from entropi.config.schema import QualityRulesConfig
from entropi.quality.analyzers.complexity import CognitiveComplexityAnalyzer
from entropi.quality.analyzers.typing import TypeHintAnalyzer
from entropi.quality.analyzers.structure import StructureAnalyzer


class TestCognitiveComplexity:
    """Tests for cognitive complexity analyzer."""

    @pytest.fixture
    def analyzer(self) -> CognitiveComplexityAnalyzer:
        return CognitiveComplexityAnalyzer()

    @pytest.fixture
    def rules(self) -> QualityRulesConfig:
        return QualityRulesConfig(max_cognitive_complexity=5)

    def test_simple_function(self, analyzer, rules) -> None:
        """Test simple function has low complexity."""
        code = '''
def add(a, b):
    return a + b
'''
        result = analyzer.analyze(code, rules)
        assert not result.violations

    def test_complex_function(self, analyzer, rules) -> None:
        """Test complex function is flagged."""
        code = '''
def complex_func(x):
    if x > 0:
        if x > 10:
            if x > 100:
                for i in range(x):
                    if i % 2 == 0:
                        print(i)
    return x
'''
        result = analyzer.analyze(code, rules)
        assert len(result.violations) > 0
        assert any("cognitive_complexity" in v["rule"] for v in result.violations)


class TestStructure:
    """Tests for structure analyzer."""

    @pytest.fixture
    def analyzer(self) -> StructureAnalyzer:
        return StructureAnalyzer()

    @pytest.fixture
    def rules(self) -> QualityRulesConfig:
        return QualityRulesConfig(max_returns_per_function=3)

    def test_too_many_returns(self, analyzer, rules) -> None:
        """Test function with too many returns is flagged."""
        code = '''
def multi_return(x):
    if x == 1:
        return 1
    if x == 2:
        return 2
    if x == 3:
        return 3
    if x == 4:
        return 4
    return 0
'''
        result = analyzer.analyze(code, rules)
        assert len(result.violations) > 0
        assert any("too_many_returns" in v["rule"] for v in result.violations)
```

---

## Checkpoint: Verification

```bash
# Run tests
pytest tests/unit/test_quality.py -v

# Test quality enforcement in generation
entropi ask "Write a function to check if a number is prime"
# Should generate code with type hints and docstrings
```

**Success Criteria:**
- [ ] Complexity analyzer detects high complexity
- [ ] Type hint analyzer flags missing hints
- [ ] Structure analyzer catches too many returns
- [ ] Regeneration loop triggers on violations
- [ ] Quality feedback is clear and actionable

---

## Next Phase

Proceed to **Implementation 10: Docker & Distribution** to finalize packaging.
