"""Native suite identities shared by test dispatch and phase validation.

Platform profiles select subsets of this registry; importing it does not load
command parsers, runners, or native build tooling.
"""

from dataclasses import dataclass
from typing import Literal


@dataclass(frozen=True)
class SuiteSpec:
    label: str
    kind: Literal["appkit", "catch2", "tooling", "lint", "winui"]
    target: str | None = None


SUITES = {
    "appkit": SuiteSpec("AppKit Smoke", "appkit", "ao_appkit_smoke"),
    "winui": SuiteSpec("WinUI Naming Audit", "winui", "aobus-winui"),
    "core": SuiteSpec("Core", "catch2", "ao_core_test"),
    "backend": SuiteSpec("Backend", "catch2", "ao_backend_test"),
    "tui": SuiteSpec("TUI", "catch2", "ao_tui_test"),
    "cli": SuiteSpec("CLI", "catch2", "ao_cli_test"),
    "gtk": SuiteSpec("GTK", "catch2", "ao_gtk_test"),
    "systemmedia": SuiteSpec("System Media", "catch2", "ao_systemmedia_test"),
    "tooling": SuiteSpec("Tooling Tests", "tooling"),
    "lint": SuiteSpec("Lint Integration", "lint", "AobusLintPlugin"),
}
