"""Portal command modules.

COMMAND_MODULES is the single registry: each module declares NAME and
REQUIRES_BUILD_ENV and registers an argparse subparser under NAME.
"""

from . import analyze, build, check, council, coverage, hygiene, name_audit, run, test, test_audit, tidy
from . import format as format_command

COMMAND_MODULES = (
    build,
    check,
    test,
    test_audit,
    name_audit,
    coverage,
    tidy,
    analyze,
    format_command,
    hygiene,
    run,
    council,
)
