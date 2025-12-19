"""Tests for quality enforcement."""

import pytest
from entropi.config.schema import QualityConfig, QualityRulesConfig
from entropi.quality.analyzers.complexity import CognitiveComplexityAnalyzer
from entropi.quality.analyzers.docstrings import DocstringAnalyzer
from entropi.quality.analyzers.structure import StructureAnalyzer
from entropi.quality.analyzers.typing import TypeHintAnalyzer
from entropi.quality.enforcer import QualityEnforcer, QualityReport


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
        code = """
def add(a, b):
    return a + b
"""
        result = analyzer.analyze(code, rules)
        assert not result.violations

    def test_complex_function(self, analyzer, rules) -> None:
        """Test complex function is flagged."""
        code = """
def complex_func(x):
    if x > 0:
        if x > 10:
            if x > 100:
                for i in range(x):
                    if i % 2 == 0:
                        print(i)
    return x
"""
        result = analyzer.analyze(code, rules)
        assert len(result.violations) > 0
        assert any("cognitive_complexity" in v["rule"] for v in result.violations)

    def test_boolean_operators_add_complexity(self, analyzer, rules) -> None:
        """Test that boolean operators increase complexity."""
        code = """
def check(a, b, c, d):
    if a and b and c and d:
        return True
    return False
"""
        result = analyzer.analyze(code, rules)
        # Should have some complexity due to if + boolean ops
        assert "check_complexity" in result.metrics

    def test_syntax_error_detected(self, analyzer, rules) -> None:
        """Test syntax errors are reported."""
        code = """
def broken(
    return 1
"""
        result = analyzer.analyze(code, rules)
        assert len(result.violations) > 0
        assert any("syntax_error" in v["rule"] for v in result.violations)

    def test_cyclomatic_complexity(self, analyzer) -> None:
        """Test cyclomatic complexity calculation."""
        rules = QualityRulesConfig(
            max_cognitive_complexity=50,
            max_cyclomatic_complexity=3,
        )
        code = """
def multi_branch(x):
    if x == 1:
        return 1
    elif x == 2:
        return 2
    elif x == 3:
        return 3
    else:
        return 0
"""
        result = analyzer.analyze(code, rules)
        assert any("cyclomatic_complexity" in v["rule"] for v in result.violations)


class TestTypeHintAnalyzer:
    """Tests for type hint analyzer."""

    @pytest.fixture
    def analyzer(self) -> TypeHintAnalyzer:
        return TypeHintAnalyzer()

    @pytest.fixture
    def rules(self) -> QualityRulesConfig:
        return QualityRulesConfig(require_type_hints=True, require_return_type=True)

    def test_properly_typed_function(self, analyzer, rules) -> None:
        """Test fully typed function passes."""
        code = """
def add(a: int, b: int) -> int:
    return a + b
"""
        result = analyzer.analyze(code, rules)
        assert not result.violations

    def test_missing_param_type(self, analyzer, rules) -> None:
        """Test missing parameter type is flagged."""
        code = """
def add(a, b: int) -> int:
    return a + b
"""
        result = analyzer.analyze(code, rules)
        assert len(result.violations) > 0
        assert any("missing_type_hint" in v["rule"] for v in result.violations)

    def test_missing_return_type(self, analyzer, rules) -> None:
        """Test missing return type is flagged."""
        code = """
def add(a: int, b: int):
    return a + b
"""
        result = analyzer.analyze(code, rules)
        assert len(result.violations) > 0
        assert any("missing_return_type" in v["rule"] for v in result.violations)

    def test_skips_dunder_methods(self, analyzer, rules) -> None:
        """Test dunder methods are skipped."""
        code = """
class Foo:
    def __init__(self, x):
        self.x = x
"""
        result = analyzer.analyze(code, rules)
        # __init__ should not require return type, and self param is skipped
        return_violations = [v for v in result.violations if "return" in v["rule"]]
        assert not return_violations

    def test_skips_self_cls(self, analyzer, rules) -> None:
        """Test self and cls params are skipped."""
        code = """
class Foo:
    def method(self, x: int) -> int:
        return x

    @classmethod
    def create(cls, x: int) -> "Foo":
        return cls(x)
"""
        result = analyzer.analyze(code, rules)
        assert not result.violations


class TestDocstringAnalyzer:
    """Tests for docstring analyzer."""

    @pytest.fixture
    def analyzer(self) -> DocstringAnalyzer:
        return DocstringAnalyzer()

    @pytest.fixture
    def rules(self) -> QualityRulesConfig:
        return QualityRulesConfig(require_docstrings=True, docstring_style="google")

    def test_function_with_docstring(self, analyzer, rules) -> None:
        """Test function with docstring passes."""
        code = '''
def add(a: int, b: int) -> int:
    """Add two numbers.

    Args:
        a: First number
        b: Second number

    Returns:
        Sum of a and b
    """
    return a + b
'''
        result = analyzer.analyze(code, rules)
        assert not result.violations

    def test_missing_function_docstring(self, analyzer, rules) -> None:
        """Test missing function docstring is flagged."""
        code = """
def add(a: int, b: int) -> int:
    return a + b
"""
        result = analyzer.analyze(code, rules)
        assert len(result.violations) > 0
        assert any("missing_docstring" in v["rule"] for v in result.violations)

    def test_missing_class_docstring(self, analyzer, rules) -> None:
        """Test missing class docstring is flagged."""
        code = """
class Foo:
    def __init__(self):
        pass
"""
        result = analyzer.analyze(code, rules)
        assert len(result.violations) > 0
        assert any("missing_docstring" in v["rule"] for v in result.violations)

    def test_skips_private_methods(self, analyzer, rules) -> None:
        """Test private methods are skipped."""
        code = '''
class Foo:
    """A class."""
    def _helper(self):
        pass
'''
        result = analyzer.analyze(code, rules)
        # Private method _helper should not require docstring
        func_violations = [v for v in result.violations if "_helper" in v["message"]]
        assert not func_violations

    def test_google_style_missing_args(self, analyzer, rules) -> None:
        """Test Google style docstring missing Args section."""
        code = '''
def add(a: int, b: int) -> int:
    """Add two numbers."""
    return a + b
'''
        result = analyzer.analyze(code, rules)
        # Should have warning about missing Args section
        assert len(result.warnings) > 0
        assert any("Args:" in w["message"] for w in result.warnings)


class TestStructureAnalyzer:
    """Tests for structure analyzer."""

    @pytest.fixture
    def analyzer(self) -> StructureAnalyzer:
        return StructureAnalyzer()

    @pytest.fixture
    def rules(self) -> QualityRulesConfig:
        return QualityRulesConfig(max_returns_per_function=3)

    def test_too_many_returns(self, analyzer, rules) -> None:
        """Test function with too many returns is flagged."""
        code = """
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
"""
        result = analyzer.analyze(code, rules)
        assert len(result.violations) > 0
        assert any("too_many_returns" in v["rule"] for v in result.violations)

    def test_too_many_parameters(self, analyzer) -> None:
        """Test function with too many parameters is flagged."""
        rules = QualityRulesConfig(max_parameters=3)
        code = """
def many_params(a, b, c, d, e):
    return a + b + c + d + e
"""
        result = analyzer.analyze(code, rules)
        assert len(result.violations) > 0
        assert any("too_many_parameters" in v["rule"] for v in result.violations)

    def test_snake_case_function_naming(self, analyzer) -> None:
        """Test non-snake_case function names are flagged."""
        rules = QualityRulesConfig(enforce_snake_case_functions=True)
        code = """
def BadFunctionName():
    pass
"""
        result = analyzer.analyze(code, rules)
        assert len(result.warnings) > 0
        assert any("naming_convention" in w["rule"] for w in result.warnings)

    def test_pascal_case_class_naming(self, analyzer) -> None:
        """Test non-PascalCase class names are flagged."""
        rules = QualityRulesConfig(enforce_pascal_case_classes=True)
        code = """
class bad_class_name:
    pass
"""
        result = analyzer.analyze(code, rules)
        assert len(result.warnings) > 0
        assert any("naming_convention" in w["rule"] for w in result.warnings)

    def test_acceptable_return_count(self, analyzer, rules) -> None:
        """Test function with acceptable returns passes."""
        code = """
def ok_returns(x):
    if x > 0:
        return 1
    return 0
"""
        result = analyzer.analyze(code, rules)
        return_violations = [v for v in result.violations if "return" in v["rule"]]
        assert not return_violations


class TestQualityEnforcer:
    """Tests for quality enforcer."""

    @pytest.fixture
    def enforcer(self) -> QualityEnforcer:
        config = QualityConfig(enabled=True)
        return QualityEnforcer(config)

    def test_analyze_good_code(self, enforcer) -> None:
        """Test analyzing good quality code."""
        code = '''
def add(a: int, b: int) -> int:
    """Add two numbers.

    Args:
        a: First number
        b: Second number

    Returns:
        Sum of a and b
    """
    return a + b
'''
        report = enforcer.analyze(code, language="python")
        assert report.passed

    def test_analyze_bad_code(self, enforcer) -> None:
        """Test analyzing bad quality code."""
        code = """
def badName(x):
    if x == 1:
        return 1
    if x == 2:
        return 2
    if x == 3:
        return 3
    if x == 4:
        return 4
    if x == 5:
        return 5
    return 0
"""
        report = enforcer.analyze(code, language="python")
        assert not report.passed
        assert len(report.violations) > 0

    def test_language_detection(self, enforcer) -> None:
        """Test language detection from filename."""
        code = "const x = 1;"
        report = enforcer.analyze(code, filename="test.js")
        # Should not fail on JS syntax in Python analyzer
        assert report.passed

    def test_extract_code_blocks(self, enforcer) -> None:
        """Test extracting code blocks from markdown."""
        content = """Here is some code:

```python
def hello():
    print("Hello")
```

And more code:

```javascript
console.log("hello");
```
"""
        blocks = enforcer.extract_code_blocks(content)
        assert len(blocks) == 2
        assert blocks[0]["language"] == "python"
        assert blocks[1]["language"] == "javascript"
        assert "def hello" in blocks[0]["code"]

    def test_disabled_enforcer(self) -> None:
        """Test disabled enforcer always passes."""
        config = QualityConfig(enabled=False)
        enforcer = QualityEnforcer(config)

        # Bad code should pass when disabled
        code = "def x(a,b,c,d,e,f,g): return 1"
        report = enforcer.analyze(code)
        assert report.passed

    def test_format_feedback(self) -> None:
        """Test formatting feedback for violations."""
        report = QualityReport(passed=False)
        report.violations = [
            {"rule": "too_many_returns", "message": "Function has 5 returns", "line": 10},
            {"rule": "missing_docstring", "message": "No docstring", "line": 1},
        ]

        feedback = report.format_feedback()
        assert "too_many_returns" in feedback
        assert "missing_docstring" in feedback
        assert "line 10" in feedback

    @pytest.mark.asyncio
    async def test_enforce_with_regeneration(self, enforcer) -> None:
        """Test enforcement with regeneration callback."""
        bad_code = """```python
def x(a,b,c,d,e,f):
    return 1
```"""

        good_code = '''```python
def add(a: int, b: int) -> int:
    """Add two numbers."""
    return a + b
```'''

        call_count = 0

        async def regenerate(feedback: str) -> str:
            nonlocal call_count
            call_count += 1
            return good_code

        final_content, report = await enforcer.enforce(bad_code, regenerate, max_attempts=3)

        # Should have called regenerate at least once
        assert call_count >= 1
        # Final content should be the good code
        assert "def add" in final_content


class TestQualityReport:
    """Tests for QualityReport."""

    def test_add_result(self) -> None:
        """Test adding analysis results."""
        from entropi.quality.analyzers.base import AnalysisResult

        report = QualityReport(passed=True)
        result = AnalysisResult()
        result.add_violation("test_rule", "Test message", line=1)

        report.add_result(result)

        assert not report.passed
        assert len(report.violations) == 1

    def test_passed_report_empty_feedback(self) -> None:
        """Test passed report has empty feedback."""
        report = QualityReport(passed=True)
        assert report.format_feedback() == ""
