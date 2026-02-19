"""Configuration for PyChess.

Uses entropic's ConfigLoader with consumer overrides:
    - app_dir_name=".pychess" — own config directory, independent of .entropic/
    - default_config_path — seeds from chess-specific defaults, not entropic's
    - global_config_dir=None — no global ~/.entropic/ layer

On first run, creates .pychess/config.local.yaml seeded from
data/default_config.yaml. Edit model paths there, then re-run.
"""

from __future__ import annotations

from pathlib import Path

from entropic import ConfigLoader, EntropyConfig

EXAMPLE_ROOT = Path(__file__).parent
PROMPTS_DIR = EXAMPLE_ROOT / "prompts"
DEFAULT_CONFIG = EXAMPLE_ROOT / "data" / "default_config.yaml"


def load_config() -> EntropyConfig:
    """Load pychess config from .pychess/config.local.yaml.

    Returns:
        Fully constructed EntropyConfig.
    """
    loader = ConfigLoader(
        project_root=EXAMPLE_ROOT,
        app_dir_name=".pychess",
        default_config_path=DEFAULT_CONFIG,
        global_config_dir=None,
    )
    return loader.load(
        cli_overrides={
            "prompts_dir": str(PROMPTS_DIR),
            "use_bundled_prompts": False,
        }
    )
