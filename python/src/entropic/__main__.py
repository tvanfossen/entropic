# SPDX-License-Identifier: Apache-2.0
"""Entry point for ``python -m entropic`` (gh#18, v2.1.5).

Mirrors the ``entropic`` console_scripts entry so downstream consumers
can invoke the CLI against an explicit interpreter rather than relying
on ``PATH`` resolution (e.g. ``[sys.executable, "-m", "entropic", ...]``).
"""

from entropic.cli import main

if __name__ == "__main__":
    main()
