# SPDX-License-Identifier: LGPL-3.0-or-later
"""Build a doxygen SQLite knowledge database from source code.

Standalone script for generating a SQLite database from doxygen comments
and optionally enriching it with curriculum/topic metadata from a YAML file.

Designed to be portable — other repos can copy this script and adjust paths
to generate their own documentation knowledge bases.

Usage:
    python build_docs_db.py --doxyfile docs/Doxyfile --output data/entropic_docs.db
    python build_docs_db.py --doxyfile docs/Doxyfile --output data/entropic_docs.db \
        --enrich data/architecture_topics.yaml

@brief Build doxygen SQLite knowledge database from source code.
@version 1
"""

from __future__ import annotations

import argparse
import json
import logging
import shutil
import sqlite3
import subprocess
import sys
from pathlib import Path

logger = logging.getLogger(__name__)


## @brief Run doxygen and return path to generated sqlite3 database.
## @param doxyfile Path to the Doxyfile.
## @param work_dir Working directory for doxygen (usually Doxyfile's parent).
## @return Path to the generated doxygen_sqlite3.db.
## @utility
## @version 1
def run_doxygen(doxyfile: Path, work_dir: Path) -> Path:
    """Run doxygen with GENERATE_SQLITE3 and return the database path.

    @brief Run doxygen and return path to generated sqlite3 database.
    @version 1
    """
    logger.info("Running doxygen: %s (cwd: %s)", doxyfile, work_dir)
    result = subprocess.run(
        ["doxygen", str(doxyfile)],
        cwd=str(work_dir),
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        logger.error("Doxygen failed:\n%s", result.stderr)
        sys.exit(1)

    # Doxygen puts sqlite3 output in <OUTPUT_DIRECTORY>/sqlite3/ by default,
    # or <OUTPUT_DIRECTORY>/<SQLITE3_OUTPUT>/ if SQLITE3_OUTPUT is set.
    base_dir = _parse_doxyfile_value(doxyfile, "OUTPUT_DIRECTORY") or "."
    sqlite_subdir = _parse_doxyfile_value(doxyfile, "SQLITE3_OUTPUT") or "sqlite3"
    db_path = work_dir / base_dir / sqlite_subdir / "doxygen_sqlite3.db"
    if not db_path.exists():
        logger.error("Expected database not found: %s", db_path)
        sys.exit(1)

    logger.info("Generated database: %s", db_path)
    return db_path


## @brief Parse a single key=value from a Doxyfile.
## @param doxyfile Path to the Doxyfile.
## @param key Key to search for.
## @return Stripped value string, or empty string if not found.
## @utility
## @version 1
def _parse_doxyfile_value(doxyfile: Path, key: str) -> str:
    """Extract a value from a Doxyfile by key.

    @brief Parse a single key=value from a Doxyfile.
    @version 1
    """
    for line in doxyfile.read_text().splitlines():
        stripped = line.strip()
        if stripped.startswith(key) and "=" in stripped:
            return stripped.split("=", 1)[1].strip()
    return ""


## @brief Copy database to output location.
## @param src Source database path.
## @param dest Destination database path.
## @utility
## @version 1
def copy_database(src: Path, dest: Path) -> None:
    """Copy the generated database to the target location.

    @brief Copy database to output location.
    @version 1
    """
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(str(src), str(dest))
    logger.info("Copied database to: %s", dest)


## @brief Add architecture_topics table from YAML enrichment file.
## @param db_path Path to the SQLite database.
## @param yaml_path Path to the architecture_topics.yaml file.
## @utility
## @version 1
def enrich_database(db_path: Path, yaml_path: Path) -> None:
    """Populate architecture_topics table from a YAML curriculum file.

    The YAML file should contain a list of topic entries with fields:
    topic, title, summary, key_files, key_concepts, prerequisite_topics,
    difficulty.

    @brief Add architecture_topics table from YAML enrichment file.
    @version 1
    """
    try:
        import yaml
    except ImportError:
        logger.error("PyYAML required for enrichment: pip install pyyaml")
        sys.exit(1)

    topics = yaml.safe_load(yaml_path.read_text())
    if not isinstance(topics, list):
        logger.error("Expected a YAML list in %s", yaml_path)
        sys.exit(1)

    conn = sqlite3.connect(str(db_path))
    _create_topics_table(conn)
    _insert_topics(conn, topics)
    conn.close()
    logger.info("Enriched database with %d topics from %s", len(topics), yaml_path)


## @brief Create the architecture_topics table if it doesn't exist.
## @param conn SQLite connection.
## @utility
## @version 1
def _create_topics_table(conn: sqlite3.Connection) -> None:
    """Create the architecture_topics table schema.

    @brief Create the architecture_topics table if it doesn't exist.
    @version 1
    """
    conn.execute("""
        CREATE TABLE IF NOT EXISTS architecture_topics (
            id INTEGER PRIMARY KEY,
            topic TEXT NOT NULL,
            title TEXT NOT NULL,
            summary TEXT NOT NULL,
            key_files TEXT NOT NULL,
            key_concepts TEXT NOT NULL,
            prerequisite_topics TEXT,
            difficulty INTEGER DEFAULT 1
        )
    """)
    conn.execute("DELETE FROM architecture_topics")
    conn.commit()


## @brief Insert topic rows from parsed YAML data.
## @param conn SQLite connection.
## @param topics List of topic dicts from YAML.
## @utility
## @version 1
def _insert_topics(conn: sqlite3.Connection, topics: list[dict]) -> None:
    """Insert topic entries into the architecture_topics table.

    @brief Insert topic rows from parsed YAML data.
    @version 1
    """
    for topic in topics:
        conn.execute(
            """INSERT INTO architecture_topics
               (topic, title, summary, key_files, key_concepts,
                prerequisite_topics, difficulty)
               VALUES (?, ?, ?, ?, ?, ?, ?)""",
            (
                topic["topic"],
                topic["title"],
                topic["summary"],
                json.dumps(topic.get("key_files", [])),
                json.dumps(topic.get("key_concepts", [])),
                json.dumps(topic.get("prerequisite_topics", [])),
                topic.get("difficulty", 1),
            ),
        )
    conn.commit()


## @brief Report database contents summary.
## @param db_path Path to the SQLite database.
## @utility
## @version 1
def report_stats(db_path: Path) -> None:
    """Print summary statistics of the generated database.

    @brief Report database contents summary.
    @version 1
    """
    conn = sqlite3.connect(str(db_path))
    tables = [
        r[0] for r in conn.execute("SELECT name FROM sqlite_master WHERE type='table'").fetchall()
    ]

    print(f"\n  Database: {db_path}")
    print(f"  Tables: {len(tables)}")

    for table in sorted(tables):
        count = conn.execute(f"SELECT COUNT(*) FROM [{table}]").fetchone()[0]
        print(f"    {table}: {count} rows")

    conn.close()


## @brief Parse CLI arguments and run the build pipeline.
## @utility
## @version 1
def main() -> None:
    """Entry point — parse args, run doxygen, optionally enrich.

    @brief Parse CLI arguments and run the build pipeline.
    @version 1
    """
    parser = argparse.ArgumentParser(
        description="Build doxygen SQLite knowledge database",
    )
    parser.add_argument(
        "--doxyfile",
        required=True,
        help="Path to Doxyfile",
    )
    parser.add_argument(
        "--output",
        required=True,
        help="Output database path",
    )
    parser.add_argument(
        "--enrich",
        help="YAML file with architecture topics for enrichment",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Enable verbose logging",
    )
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(levelname)s: %(message)s",
    )

    doxyfile = Path(args.doxyfile).resolve()
    output = Path(args.output).resolve()

    if not doxyfile.exists():
        logger.error("Doxyfile not found: %s", doxyfile)
        sys.exit(1)

    # Run doxygen from the Doxyfile's directory (paths are relative to it)
    generated_db = run_doxygen(doxyfile, doxyfile.parent)
    copy_database(generated_db, output)

    if args.enrich:
        enrich_path = Path(args.enrich).resolve()
        if not enrich_path.exists():
            logger.error("Enrichment file not found: %s", enrich_path)
            sys.exit(1)
        enrich_database(output, enrich_path)

    report_stats(output)


if __name__ == "__main__":
    main()
