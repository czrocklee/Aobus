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


WINUI_CHECK_TARGET = "//app/windows-winui:ao_winui_automation_probe"

SUITES = {
    "appkit": SuiteSpec("AppKit Smoke", "appkit", "//app/macos-appkit:ao_appkit_smoke"),
    "winui": SuiteSpec("WinUI Naming Audit", "winui", WINUI_CHECK_TARGET),
    "core": SuiteSpec("Core", "catch2", "//:core_tests"),
    "application": SuiteSpec("Application", "catch2", "//:application_tests"),
    "backend": SuiteSpec("Backend", "catch2", "//test:backend_test"),
    "tui": SuiteSpec("TUI", "catch2", "//test:tui_test"),
    "cli": SuiteSpec("CLI", "catch2", "//test:cli_test"),
    "gtk": SuiteSpec("GTK", "catch2", "//test:gtk_test"),
    "systemmedia": SuiteSpec("System Media", "catch2", "//test:systemmedia_test"),
    "tooling": SuiteSpec("Tooling Tests", "tooling"),
    "lint": SuiteSpec("Lint Integration", "lint", "//test/lint:lint_test"),
}
