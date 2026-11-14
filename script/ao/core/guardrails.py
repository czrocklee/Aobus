"""CMake-independent completion audits shared by native build frontends.

These are lexical source-policy checks, not C++ parsers. Raw rules deliberately
see comments and strings; code-view rules retain the historical comment-only
filter (including its treatment of comment markers in string literals).
"""

import json
import os
import re
import sys
from collections.abc import Iterable
from dataclasses import asdict, dataclass
from pathlib import Path

RAW_SUFFIXES = frozenset((".h", ".hh", ".hpp", ".hxx", ".c", ".cc", ".cpp", ".cxx", ".mm", ".inl", ".ipp", ".def"))
NARROW_SUFFIXES = frozenset((".h", ".hpp", ".cpp"))
PRODUCTION_ROOTS = ("app", "include", "lib", "tool")


class AuditInputError(ValueError):
    """A required source root or audit selection is invalid."""


@dataclass(frozen=True)
class Finding:
    audit: str
    rule: str
    path: str
    detail: str

    def __str__(self) -> str:
        return f"{self.rule}: {self.path}: {self.detail}"


@dataclass(frozen=True)
class AuditResult:
    errors: tuple[Finding, ...] = ()
    warnings: tuple[Finding, ...] = ()

    @property
    def ok(self) -> bool:
        return not self.errors


@dataclass(frozen=True)
class Rule:
    name: str
    roots: tuple[str, ...]
    forbidden: str
    allowed: str = ""
    exclude: str = ""


@dataclass(frozen=True)
class Audit:
    name: str
    rules: tuple[Rule, ...] = ()
    condition: str = ""
    mode: str = "enforce"


def _text(path: Path) -> str:
    # Do not normalize CRLF: the raw scanners operate on the bytes as written.
    return path.read_bytes().decode("utf-8", errors="surrogateescape")


def _absolute(path: Path) -> Path:
    # Normalize without resolving symlinks: owner exclusions name checkout paths.
    return Path(os.path.abspath(path))


def source_files(root: Path, suffixes: frozenset[str] = RAW_SUFFIXES) -> list[Path]:
    """Expand a required raw root; explicit files are scanned at any suffix."""
    if root.is_dir():
        root = _absolute(root)
        # CMake GLOB is case-insensitive on Windows and macOS, even when the
        # mounted filesystem itself is case-sensitive.
        fold_case = sys.platform in ("win32", "darwin")
        return sorted(
            path
            for path in root.rglob("*")
            if path.is_file() and (path.suffix.lower() if fold_case else path.suffix) in suffixes
        )
    if root.exists():
        return [root]
    raise AuditInputError(f"root does not exist: {root}")


def _directory(root: Path | str | None, name: str) -> Path:
    if root is None or str(root) == "":
        raise AuditInputError(f"{name} not specified")
    root = _absolute(Path(root))
    if not root.is_dir():
        raise AuditInputError(f"{name} is not a directory: {root}")
    return root


def _scan_rules(root: Path, audit: str, rules: tuple[Rule, ...], *, deduplicate: bool = False) -> list[Finding]:
    findings = []
    contents: dict[Path, str] = {}
    for rule in rules:
        seen: set[Path] = set()
        for relative in rule.roots:
            for path in source_files(root / relative):
                if deduplicate and path in seen:
                    continue
                seen.add(path)
                if rule.exclude and re.search(rule.exclude, path.as_posix()):
                    continue
                if path not in contents:
                    contents[path] = _text(path)
                content = contents[path]
                if rule.allowed:
                    content = re.sub(rule.allowed, "", content)
                match = re.search(rule.forbidden, content)
                if match:
                    findings.append(Finding(audit, rule.name, path.relative_to(root).as_posix(), repr(match[0])))
    return findings


def scan_forbidden(
    roots: Iterable[Path],
    *,
    forbidden: str = r'#[ \t]*(include|import)[ \t]*[<"](gtkmm|gdkmm|giomm|glibmm|gtk|gdk|gio|glib)/',
    allowed: str = "",
    exclude: str = "",
    mode: str = "enforce",
) -> AuditResult:
    """Generic adapter API; report findings warn, while invalid inputs still fail.

    Keep root overlap (and one finding per file per root), as the CMake scanner
    did. Directory suffixes are broad; an explicit file has no suffix filter.
    """
    if mode not in ("enforce", "report"):
        raise AuditInputError(f"MODE must be 'enforce' or 'report' (got '{mode}')")
    roots = tuple(roots)
    if not roots:
        raise AuditInputError("ROOTS not specified")
    findings = []
    for root in roots:
        for path in source_files(_absolute(root)):
            if exclude and re.search(exclude, path.as_posix()):
                continue
            content = _text(path)
            if allowed:
                content = re.sub(allowed, "", content)
            match = re.search(forbidden, content)
            if match:
                findings.append(Finding("forbidden", "Forbidden dependency in", path.as_posix(), repr(match[0])))
    return AuditResult(warnings=tuple(findings)) if mode == "report" else AuditResult(errors=tuple(findings))


def code_lines(path: Path) -> list[tuple[str, str]]:
    """Return original/code lines, carrying block-comment state across lines."""
    result = []
    in_comment = False
    for line in _text(path).splitlines():
        rest = line
        code = ""
        while rest:
            if in_comment:
                at = rest.find("*/")
                if at < 0:
                    break
                rest = rest[at + 2 :]
                code += " "
                in_comment = False
            else:
                block, eol = rest.find("/*"), rest.find("//")
                if eol >= 0 and (block < 0 or eol < block):
                    code += rest[:eol] + " "
                    break
                if block < 0:
                    code += rest
                    break
                code += rest[:block]
                rest = rest[block + 2 :]
                in_comment = True
        result.append((line, code))
    return result


def _code_text(lines: list[tuple[str, str]]) -> str:
    return re.sub(r"[ \t\r\n]+", " ", "".join(code + " " for _, code in lines))


# These exclusions name composition roots, never filename-shaped exemptions.
GTK_COMPOSITION_ROOTS = frozenset(
    (
        "app/LibraryWindowLifecycle.cpp",
        "app/MainWindow.h",
        "app/MainWindow.cpp",
        "app/ShellLayoutController.h",
        "app/ShellLayoutController.cpp",
        "layout/component/ComponentRegistrations.h",
        "layout/component/playback/PlaybackRegistrations.cpp",
        "layout/component/status/StatusRegistrations.cpp",
        "layout/component/track/TrackRegistrations.cpp",
        "layout/runtime/LayoutRuntime.h",
        "layout/runtime/LayoutRuntime.cpp",
        "list/ListNavigationController.h",
        "list/ListNavigationController.cpp",
    )
)
WINUI_COMPOSITION_ROOTS = frozenset(
    (
        "MainWindow.xaml.h",
        "MainWindow.xaml.cpp",
        "app/LibrarySession.h",
        "app/LibrarySession.cpp",
        "app/LibraryWindowSession.h",
        "app/LibraryWindowSession.cpp",
        "layout/ShellBuilder.h",
        "layout/ShellBuilder.cpp",
        "playback/MainWindowPlayback.cpp",
        "shell/MainWindowShell.cpp",
        "track/MainWindowTrack.cpp",
    )
)


def audit_frontend(kind: str, root: Path | str | None) -> AuditResult:
    if kind not in ("gtk-leaf", "winui-leaf", "winui-events", "winui-state"):
        raise AuditInputError(f"unknown frontend audit: {kind}")
    root = _directory(root, "ROOT")
    findings = []
    for path in source_files(root, NARROW_SUFFIXES):
        relative = path.relative_to(root).as_posix()
        lines = code_lines(path)
        message = ""
        if kind in ("gtk-leaf", "winui-leaf"):
            owners = GTK_COMPOSITION_ROOTS if kind == "gtk-leaf" else WINUI_COMPOSITION_ROOTS
            if relative in owners:
                continue
            pattern = "AppRuntime" if kind == "gtk-leaf" else "LibrarySession|AppRuntime"
            if any(re.search(pattern, code) for _, code in lines):
                message = "reaches a high-authority dependency"
        elif kind == "winui-events":
            if any(re.search(r"AppWindow\(\)\.Changed\(winrt::auto_revoke", code) for _, code in lines):
                message = "uses auto_revoke with AppWindow: AppWindow does not support weak references"
            elif any("event_token" in code for _, code in lines):
                message = "holds a raw event_token"
            # Wrapped receiver callbacks and every capture list naming this
            # need scoped revocation; receiverless self-registration is exempt.
            elif re.search(r"\.[A-Z][A-Za-z0-9_]*\( ?(\{this,|\[[^]]*this)", _code_text(lines)):
                message = "registers an owner callback without auto_revoke"
        elif kind == "winui-state":
            # This legacy-state guard is intentionally raw, including comments.
            content = _text(path)
            for identifier in (
                "onShellStateChanged",
                "onTrackListChanged",
                "onWindowActivityChanged",
                "onStatusMessageChanged",
            ):
                if identifier in content:
                    message = f"restores the old '{identifier}' push contract"
                    break
            if not message and re.search(r"(ctx|context|generation)[.]observers|observers[.]push_back", content):
                message = "restores raw component fan-out"
        if message:
            findings.append(Finding(kind, kind, path.as_posix(), message))
    return AuditResult(errors=tuple(findings))


UIMODEL_LAYOUT = (
    r"^(FrameClock\.h|field/[^/]+\.(h|hpp|cpp)|input/[^/]+\.(h|hpp|cpp)|preference/[^/]+\.(h|hpp|cpp)|"
    r"presentation/[^/]+\.(h|hpp|cpp)|layout/(document|component|shell)/[^/]+\.(h|hpp|cpp)|"
    r"library/(list|presentation|track|detail|property|task)/[^/]+\.(h|hpp|cpp)|"
    r"playback/(command|now-playing|transport|seek|output|quality|soul)/[^/]+\.(h|hpp|cpp)|"
    r"status/activity/[^/]+\.(h|hpp|cpp))$"
)
FRONTEND_NAME = r"^(Windows|WinUi|WinUI|Gtk|GTK|Linux|Tui|Cli|Cocoa|AppKit|Qt)([._A-Z0-9]|$)"
FRONTEND_VOCABULARY = (
    r"(winrt::|Xaml|gtkmm|Gtk::|Gdk::|Glib::|Gio::|Pango::|Cairo::|ftxui::|ftxui/|"
    r'#[ \t]*(include|import)[ \t]*[<"](AppKit|Cocoa|Foundation|CoreGraphics)/|'
    r"@[ \t]*import[ \t]+(AppKit|Cocoa|Foundation|CoreGraphics)[ \t]*([.]|;)|"
    r"(^|[^A-Za-z0-9_])((NS|CG)[A-Z_]|kCG[A-Z])[A-Za-z0-9_]*)"
)


def audit_uimodel(
    kind: str, public: Path | str | None, source: Path | str | None, test: Path | str | None
) -> AuditResult:
    if kind not in ("uimodel-neutrality", "uimodel-organization"):
        raise AuditInputError(f"unknown UIModel audit: {kind}")
    roots = tuple(
        _directory(path, name)
        for path, name in zip((public, source, test), ("PUBLIC_ROOT", "SOURCE_ROOT", "TEST_ROOT"), strict=True)
    )
    findings = []
    for root, label in zip(roots, ("public", "source", "test"), strict=True):
        suffixes = NARROW_SUFFIXES | {".mm"} if kind == "uimodel-neutrality" else NARROW_SUFFIXES
        for path in source_files(root, frozenset(suffixes)):
            relative = path.relative_to(root).as_posix()
            message = ""
            if kind == "uimodel-neutrality":
                if re.search(FRONTEND_NAME, path.name):
                    message = f"{label} file names a frontend, so it is that frontend's own"
                elif re.search(FRONTEND_VOCABULARY, _code_text(code_lines(path))):
                    message = f"{label} file speaks a frontend's vocabulary"
            elif kind == "uimodel-organization":
                if not re.search(UIMODEL_LAYOUT, relative):
                    message = f"{label} file is outside the final UIModel folder layout"
                elif label == "test" or (label == "public" and path.suffix in (".h", ".hpp")):
                    for line in _text(path).splitlines():
                        code = re.sub(r"//.*$", "", line)
                        match = re.search(r"^[ \t]*namespace[ \t]+ao::uimodel::([^ \t{;]+)", code)
                        if match and match[1] != ("test" if label == "test" else "detail"):
                            message = f"{label} UIModel file declares a feature namespace: {line}"
                            break
            if message:
                findings.append(Finding(kind, kind, path.as_posix(), message))
    return AuditResult(errors=tuple(findings))


def _production_rule(name: str, forbidden: str, *, exclude: str = "", allowed: str = "", tests: bool = False) -> Rule:
    return Rule(name, (*PRODUCTION_ROOTS, "test") if tests else PRODUCTION_ROOTS, forbidden, allowed, exclude)


def _boundary_audits(root: Path) -> tuple[Audit, ...]:
    return (
        Audit("ao_no_c_assert_check", (_production_rule("c_assert", r"(^|[^A-Za-z0-9_])assert[ \t\r\n]*\("),)),
        Audit(
            "ao_no_raw_gsl_contract_check",
            (_production_rule("raw_gsl_contract", r"(^|[^A-Za-z0-9_])gsl_(Expects|Ensures|Assert)[ \t\r\n]*\("),),
        ),
        Audit(
            "ao_no_unconditional_terminal_check",
            (
                _production_rule(
                    "unconditional_terminal",
                    r"(AO_(EXPECTS|ENSURES|INVARIANT)[ \t\r\n]*\([ \t\r\n]*false([^A-Za-z0-9_]|$)|"
                    r"(^|[^A-Za-z0-9_])std::unreachable[ \t\r\n]*\()",
                ),
            ),
        ),
        Audit(
            "ao_no_removed_exception_surface_check",
            (
                _production_rule(
                    "removed_exception_surface",
                    r"(ao/Exception\.h|(^|[^A-Za-z0-9_])(ExceptionFormat|throwException)([^A-Za-z0-9_]|$))",
                ),
            ),
        ),
        # Qualified/global only: transaction.abort() is a legitimate method.
        # Imported/unqualified standard calls remain the independent AST guard's job.
        Audit(
            "ao_no_raw_fatal_primitive_check",
            (
                _production_rule(
                    "raw_fatal_primitive",
                    r"(^|[^A-Za-z0-9_])(std::(terminate|abort|quick_exit|_Exit)|"
                    r"::(terminate|abort|quick_exit|_Exit)|_Exit)[ \t\r\n]*\(",
                    exclude=r".*/lib/utility/Fatal\.cpp$",
                ),
            ),
        ),
        Audit(
            "ao_media_boundary_check",
            (
                Rule(
                    "media",
                    ("include/ao/media", "lib/media"),
                    r'#[ \t]*include[ \t]*[<"](ao/(audio|library|rt|uimodel)/|runtime/|gtkmm/|gdkmm/|giomm/|glibmm/)',
                ),
            ),
        ),
        Audit(
            "ao_library_media_file_boundary_check",
            (
                Rule(
                    "library_media_file",
                    ("include/ao/library", "lib/library"),
                    r'#[ \t]*include[ \t]*[<"]ao/media/file/',
                ),
            ),
        ),
        Audit(
            "ao_library_logical_writer_boundary_check",
            (
                _production_rule(
                    "physical_access",
                    r'#[ \t]*include[ \t]*[<"][^">]*PhysicalStoreAccess\.h[>"]|'
                    r"(^|[^A-Za-z0-9_])(class|struct)[ \t\r\n]+([A-Za-z_][A-Za-z0-9_]*::)*"
                    r"PhysicalStoreAccess([ \t\r\n]+final)?[ \t\r\n]*\{|"
                    r"(^|[^A-Za-z0-9_])PhysicalStoreAccess[ \t\r\n]*::",
                    exclude=r"/lib/library/PhysicalStoreAccess\.h$",
                ),
                _production_rule(
                    "track_encoding",
                    r'#[ \t]*include[ \t]*[<"][^">]*TrackWrite\.h[>"]|'
                    r"(^|[^A-Za-z0-9_])(createPreparedTrackRecord|updatePrepared(Hot|Cold)?TrackRecord)[ \t\r\n]*\(",
                    exclude=r"/lib/library/(TrackWrite\.(h|cpp)|TrackWriter\.cpp)$",
                ),
                _production_rule(
                    "commit_failure_injection",
                    r"(^|[^A-Za-z0-9_])optInjectedCommitFailure([^A-Za-z0-9_]|$)",
                    exclude=r"/(include/ao/library/WriteTransaction\.h|lib/library/WriteTransaction\.cpp)$",
                ),
                _production_rule(
                    "open_validation_metrics",
                    r'#[ \t]*include[ \t]*[<"][^">]*OpenValidationMetrics\.h[>"]|'
                    r"(^|[^A-Za-z0-9_])(OpenValidationMetrics|resetOpenValidationMetrics|"
                    r"recordOpenValidationNamedDatabaseOpen|recordOpenValidationTrackRow|"
                    r"recordOpenValidationManifestPointRead|openValidationMetrics)([^A-Za-z0-9_]|$)",
                    exclude=r"/lib/library/(MusicLibrary|OpenValidationMetrics)\.(h|cpp)$",
                ),
                # Tests can alias this definition through PhysicalStoreAccess.h,
                # so match the identifier after removing legal forward/friend
                # declarations. Anchor owners to this checkout: a test fixture's
                # lib/library directory is not an implementation owner.
                _production_rule(
                    "write_transaction_access",
                    r"(^|[^A-Za-z0-9_])WriteTransactionAccess([^A-Za-z0-9_]|$)",
                    allowed=(
                        r"(friend[ \t\r\n]+)?class[ \t\r\n]+(::)?([A-Za-z_][A-Za-z0-9_]*::)*"
                        r"WriteTransactionAccess[ \t\r\n]*;"
                    ),
                    exclude="^" + re.escape((root / "lib/library").as_posix()) + r"/(detail/)?[^/]+\.(h|cpp)$",
                    tests=True,
                ),
            ),
        ),
        Audit(
            "ao_lmdb_read_fault_injection_boundary_check",
            (
                _production_rule(
                    "read_fault_injection",
                    r'#[ \t]*include[ \t]*[<"][^">]*ReadFaultInjection\.h[>"]|'
                    r"(^|[^A-Za-z0-9_])ReadFaultInjection([^A-Za-z0-9_]|$)",
                    exclude=r"/lib/lmdb/detail/(ReadFaultInjection\.h|ThrowError\.cpp)$",
                ),
            ),
        ),
        Audit(
            "ao_lmdb_database_open_admission_probe_boundary_check",
            (
                _production_rule(
                    "database_open_admission_probe",
                    r'#[ \t]*include[ \t]*[<"][^">]*DatabaseOpenAdmissionProbe\.h[>"]|'
                    r"(^|[^A-Za-z0-9_])DatabaseOpenAdmissionProbe([^A-Za-z0-9_]|$)",
                    exclude=r"/lib/lmdb/(Transaction\.cpp|detail/DatabaseOpenAdmissionProbe\.h)$",
                ),
            ),
        ),
        Audit(
            "ao_lmdb_unvalidated_database_boundary_check",
            (
                _production_rule(
                    "unvalidated_database",
                    r'#[ \t]*include[ \t]*[<"][^">]*UnvalidatedDatabase\.h[>"]|'
                    r"(^|[^A-Za-z0-9_])UnvalidatedDatabase([^A-Za-z0-9_]|$)",
                    exclude=r"/lib/(lmdb/(Database\.cpp|detail/UnvalidatedDatabase\.h)|library/MusicLibrary\.cpp)$",
                ),
            ),
        ),
        Audit(
            "ao_gtk_field_grid_operation_probe_boundary_check",
            (
                _production_rule(
                    "field_grid_operation_probe",
                    r'#[ \t]*include[ \t]*[<"][^">]*TrackFieldGridOperationProbe\.h[>"]|'
                    r"(^|[^A-Za-z0-9_])TrackFieldGridOperationProbe([^A-Za-z0-9_]|$)",
                    exclude=r"/app/linux-gtk/layout/component/track/(TrackFieldGridComponent\.cpp|TrackFieldGridOperationProbe\.h)$",
                ),
            ),
            condition="gtk",
        ),
    )


def architecture_rules() -> tuple[Rule, ...]:
    audio = r"ao/audio/(Player|Backend[.]h|BackendProvider[.]h|Engine|NullBackend|backend/|detail/)"
    write = r"(^|[^A-Za-z0-9_])(WriteTransaction|WritableMusicLibrary)([^A-Za-z0-9_]|$)"
    commands = r"(\.|->)[ \t\r\n]*commands[ \t\r\n]*\("
    playback_commands = (
        r"((^|[^A-Za-z0-9_])(_?[Pp]layback|[Pp]laybackPtr)|"
        r"[.>][ \t\r\n]*[Pp]layback[ \t\r\n]*\([ \t\r\n]*\))[ \t\r\n]*(\.|->)[ \t\r\n]*commands[ \t\r\n]*\("
    )
    comments = r"//[^\r\n]*|/[*]([^*]|[*]+[^*/])*[*]+/"
    frontends = (
        "app/include/ao/desktop",
        "app/include/ao/systemmedia",
        "app/desktop",
        "app/linux-gtk",
        "app/systemmedia",
        "app/macos-appkit",
        "app/windows-winui",
        "app/tui",
    )
    return (
        Rule(
            "application_signal",
            ("app", "test"),
            r'#[ \t]*include[ \t]*[<"]ao/rt/(Signal|Subscription)[.]h[>"]|'
            r"(^|[^A-Za-z0-9_:])((::)?ao::rt::|rt::)(Signal|Subscription)([^A-Za-z0-9_]|$)",
        ),
        Rule(
            "runtime_write_transaction",
            ("app/runtime",),
            r"(^|[^A-Za-z0-9_])writeTransaction[ \t\r\n]*\(",
            exclude=r"/app/runtime/library/(LibraryWriteLane|LibraryYamlImporter|ScanApplyOperation)[.]cpp$",
        ),
        Rule(
            "playback_internal",
            (
                "app/include/ao/rt",
                "app/include/ao/uimodel",
                "app/include/ao/desktop",
                "app/include/ao/systemmedia",
                "app/uimodel",
                "app/desktop",
                "app/linux-gtk",
                "app/systemmedia",
                "app/macos-appkit",
                "app/tui",
                "app/windows-winui",
                "app/cli",
            ),
            r"(^|[^A-Za-z0-9_])(PlaybackTransport|PlaybackSuccession)([^A-Za-z0-9_]|$)",
        ),
        Rule(
            "lower_presentation_vocabulary",
            ("include", "lib", "app/include/ao/rt", "app/runtime"),
            r'"(ao-activity-status[A-Za-z0-9_.-]*|[A-Za-z0-9_.-]+-symbolic|'
            r"Unknown (Artist|Album|Year|Genre|Composer|Conductor|Ensemble|Work)|"
            r"Modern Linux audio server with low latency|"
            r"Advanced Linux Sound Architecture [(]Direct Hardware Access[)]|"
            r'Windows Audio Session API|WASAPI render endpoint|Shared Mode|Exclusive Mode|logical operator)"',
        ),
        Rule("track_field_copy", ("app/include/ao/rt/TrackField.h",), r"std::string_view[ \t]+label[ \t]*[;{]"),
        Rule(
            "track_preset_copy",
            ("app/include/ao/rt/TrackPresentation.h",),
            r"std::string_view[ \t]+(label|description)[ \t]*[;{]",
        ),
        Rule(
            "backend_descriptor_copy",
            ("include/ao/audio/BackendProvider.h",),
            r"std::string[ \t]+(name|description|iconName)[ \t]*[;{]",
        ),
        Rule(
            "completion_detail_copy",
            ("app/include/ao/rt/completion/CompletionItem.h",),
            r"std::string[ \t]+detail[ \t]*[;{]",
        ),
        Rule(
            "managed_state_mechanism",
            (
                "app/include",
                "app/runtime",
                "app/uimodel",
                "app/desktop",
                "app/linux-gtk",
                "app/systemmedia",
                "app/macos-appkit",
                "app/tui",
                "app/windows-winui",
            ),
            r"ao/yaml/Reflect[.]h|namespace[ \t\r\n]+ao::yaml",
        ),
        Rule(
            "core_yaml_domain",
            ("include/ao/yaml",),
            r"#[ \t]*include[ \t]*[<\"]ao/(CoreIds[.]h|rt/|uimodel/)|APP_LOG_|#[ \t]*include[ \t]*[<\"]spdlog/",
        ),
        Rule(
            "runtime_public",
            ("app/include/ao/rt",),
            r'(#[ \t]*include[ \t]*[<"](runtime/|ao/(lmdb/|'
            r"library/(MusicLibrary|TrackStore|ListStore|ResourceStore|DictionaryStore|FileManifestStore|TrackView|ListView)|"
            r"audio/(Player|Backend[.]h|BackendProvider[.]h|Engine|NullBackend|backend/|detail/))))|" + write,
        ),
        Rule(
            "runtime_mechanism_surface",
            ("app/include/ao/rt",),
            r"(^|[^A-Za-z0-9_])(AllTracksSource|IndexedTrackSequence|ListOrderSource|SmartListEvaluator|SmartListSource|LibraryYamlExporter|LibraryYamlImporter)([^A-Za-z0-9_]|$)",
        ),
        Rule(
            "uimodel_platform",
            ("app/include/ao/uimodel", "app/uimodel"),
            r'(#[ \t]*(include|import)[ \t]*[<"]'
            r"(gtkmm|gdkmm|giomm|glibmm|gtk|gdk|gio|glib|AppKit|Cocoa|Foundation|CoreGraphics)/)|"
            r"(@[ \t\r\n]*import[ \t\r\n]+(AppKit|Cocoa|Foundation|CoreGraphics)[ \t\r\n]*([.]|;))|"
            r'("(ao-activity-status[A-Za-z0-9_.-]*|[A-Za-z0-9_.-]+-symbolic)")',
            allowed=comments,
        ),
        Rule(
            "uimodel_core",
            ("app/include/ao/uimodel", "app/uimodel"),
            r"(#[ \t]*include[ \t]*[<\"](ao/(lmdb/|library/)|" + audio + r"))|" + write,
            allowed=(
                r"#[ \t]*include[ \t]*(<ao/library/(RecordingDate|Credits)[.]h>|"
                r'"ao/library/(RecordingDate|Credits)[.]h")'
            ),
        ),
        Rule(
            "system_media_frontend",
            ("app/include/ao/systemmedia", "app/systemmedia"),
            r'#[ \t]*(include|import)[ \t]*[<"]((gtkmm|gdkmm|gtk|gdk)(/|[.]h[>"])|ftxui/|'
            r"(winrt/)?[Mm]icrosoft[.][Uu][Ii][.]|(linux-gtk|tui|windows-winui|macos-appkit)/)",
            allowed=comments,
        ),
        Rule(
            "system_media_reverse",
            (
                "app/include/ao/rt",
                "app/include/ao/uimodel",
                "app/include/ao/desktop",
                "app/include/ao/i18n",
                "app/runtime",
                "app/uimodel",
                "app/desktop",
                "app/i18n",
                "app/cli",
            ),
            r'#[ \t]*(include|import)[ \t]*[<"](ao/)?systemmedia/',
            allowed=comments,
        ),
        Rule(
            "frontend_core",
            frontends,
            r'(#[ \t]*include[ \t]*[<"](ao/rt/CoreRuntime[.]h|ao/lmdb/|'
            r"ao/library/(MusicLibrary|TrackStore|ListStore|ResourceStore|DictionaryStore|FileManifestStore|TrackView|ListView)))|"
            + write
            + r"|(^|[^A-Za-z0-9_])LibraryCommands([^A-Za-z0-9_]|$)|"
            + commands,
            allowed=playback_commands,
        ),
        Rule("frontend_library_path", (*frontends, "app/cli"), r'"([.]aobus|data[.]mdb)'),
        Rule("cli_localization", ("app/cli",), r'#[ \t]*include[ \t]*[<"]ao/i18n/'),
    )


def architecture_self_test() -> None:
    """Retain the script-mode smoke contract without a second rule inventory."""
    examples = {
        "application_signal": ("rt::Signal", "rt::SignalRouter"),
        "runtime_write_transaction": ("library.writeTransaction()", "library.readTransaction()"),
        "playback_internal": ("PlaybackTransport", "PlaybackTransportSnapshot"),
        "lower_presentation_vocabulary": ('"folder-symbolic"', '"folder"'),
        "track_field_copy": ("std::string_view label;", "MessageId label;"),
        "track_preset_copy": ("std::string_view description{};", "MessageId description{};"),
        "backend_descriptor_copy": ("std::string iconName;", "std::string id;"),
        "completion_detail_copy": ("std::string detail;", "std::string display;"),
        "managed_state_mechanism": ("#include <ao/yaml/Reflect.h>", "#include <ao/yaml/RymlAdapter.h>"),
        "core_yaml_domain": ("#include <ao/CoreIds.h>", "#include <ao/utility/Path.h>"),
        "runtime_public": ("#include <ao/library/MusicLibrary.h>", "#include <ao/rt/LibrarySnapshot.h>"),
        "runtime_mechanism_surface": ("class SmartListEvaluator;", "class TrackSourceCache;"),
        "uimodel_platform": ("#include <gtkmm/widget.h>", "#include <ao/uimodel/layout/LayoutSchema.h>"),
        "uimodel_core": ("#include <ao/library/MusicLibrary.h>", "#include <ao/library/RecordingDate.h>"),
        "system_media_frontend": ("#include <gtkmm/widget.h>", "#include <giomm/file.h>"),
        "system_media_reverse": ("#include <ao/systemmedia/linux/MprisBridge.h>", "#include <ao/rt/AppRuntime.h>"),
        "frontend_core": ("runtime.library().commands()", "runtime.playback().commands()"),
        "frontend_library_path": ('"data.mdb"', '"library.db"'),
        "cli_localization": ("#include <ao/i18n/MessageCatalog.h>", "#include <ao/rt/CoreRuntime.h>"),
    }
    rules = architecture_rules()
    if examples.keys() != {rule.name for rule in rules}:
        raise AuditInputError("Architecture rule self-test coverage differs from registered rules")
    for rule in rules:
        for sample, rejected in zip(examples[rule.name], (True, False), strict=True):
            content = re.sub(rule.allowed, "", sample) if rule.allowed else sample
            if bool(re.search(rule.forbidden, content)) != rejected:
                raise AuditInputError(f"Architecture rule '{rule.name}' misclassifies '{sample}'")


def audit_architecture_rules(root: Path) -> AuditResult:
    root = _directory(root, "AOBUS_SOURCE_DIR")
    name = "ao_application_architecture_audit"
    findings = _scan_rules(root, name, architecture_rules(), deduplicate=True)
    for legacy in (
        "app/include/ao/yaml/ConfigTraits.h",
        "app/include/ao/rt/Signal.h",
        "app/include/ao/rt/Subscription.h",
        "app/include/ao/rt/PlaybackService.h",
        "app/include/ao/rt/PlaybackSequenceService.h",
    ):
        if (root / legacy).exists():
            findings.append(Finding(name, "removed_surface", legacy, "file must remain absent"))
    unsupported = frozenset((".c", ".cc", ".cxx", ".c++", ".hh", ".hxx", ".m", ".inl", ".ipp"))
    for relative in (*PRODUCTION_ROOTS, "test"):
        directory = root / relative
        if directory.is_dir():
            for path in source_files(directory, unsupported):
                findings.append(
                    Finding(
                        name,
                        "unsupported_cpp_suffix",
                        path.relative_to(root).as_posix(),
                        "use .cpp, .mm, .h, .hpp, or .def",
                    )
                )
    return AuditResult(errors=tuple(findings))


def inventory(source_root: Path, *, gtk: bool = True) -> tuple[Audit, ...]:
    """The enabled completion inventory; only the GTK probe is conditional.

    Architecture policy still scans all frontend source roots on every host,
    independently of which frontend binaries that build enables.
    """
    audits = (*_boundary_audits(_absolute(source_root)), Audit("ao_application_architecture_audit"))
    return tuple(audit for audit in audits if gtk or audit.condition != "gtk")


def _run_audit(root: Path, audit: Audit) -> AuditResult:
    if audit.name != "ao_application_architecture_audit":
        return AuditResult(errors=tuple(_scan_rules(root, audit.name, audit.rules)))
    results = [audit_architecture_rules(root)]
    public, source, test = (root / relative for relative in ("app/include/ao/uimodel", "app/uimodel", "test/uimodel"))
    for kind in ("uimodel-organization", "uimodel-neutrality"):
        results.append(audit_uimodel(kind, public, source, test))
    # These source contracts have never been conditional on enabled binaries.
    for kind, relative in (
        ("winui-events", "app/windows-winui"),
        ("winui-state", "app/windows-winui"),
        ("winui-leaf", "app/windows-winui"),
        ("gtk-leaf", "app/linux-gtk"),
    ):
        results.append(audit_frontend(kind, root / relative))
    return AuditResult(
        errors=tuple(
            Finding(
                audit.name,
                finding.rule,
                Path(finding.path).relative_to(root).as_posix() if Path(finding.path).is_absolute() else finding.path,
                finding.detail,
            )
            for result in results
            for finding in result.errors
        )
    )


def run_audits(source_root: Path, names: Iterable[str] | None = None, *, gtk: bool = True) -> AuditResult:
    """Run selected enabled audits, fail closed on missing roots or unknown names."""
    root = _directory(source_root, "source_root")
    audits = {audit.name: audit for audit in inventory(root, gtk=gtk)}
    selected = tuple(audits) if names is None else tuple(names)
    unknown = set(selected) - audits.keys()
    if unknown:
        raise AuditInputError("unknown or disabled audit(s): " + ", ".join(sorted(unknown)))
    errors: list[Finding] = []
    warnings: list[Finding] = []
    for name in selected:
        try:
            result = _run_audit(root, audits[name])
        except (AuditInputError, OSError, re.error) as exc:
            result = AuditResult(errors=(Finding(name, "audit_input", root.as_posix(), str(exc)),))
        errors.extend(result.errors)
        warnings.extend(result.warnings)
    return AuditResult(tuple(errors), tuple(warnings))


@dataclass(frozen=True)
class CompletionResult:
    errors: tuple[Finding, ...]
    warnings: tuple[Finding, ...]
    report_path: Path

    @property
    def exit_code(self) -> int:
        return 1 if self.errors else 0


def run(source_root: Path, output_dir: Path, platform: str, *, gtk: bool | None = None) -> CompletionResult:
    """Run the complete enabled inventory and replace its per-run JSON receipt.

    The receipt is output_dir/guardrails.json (schema_version 1); no build tree,
    CMake subprocess, repository Git state, or external dependencies are needed.
    Invalid selection/root input fails closed; findings and input failures are
    recorded alike. Output-directory I/O errors propagate to the caller.
    """
    if platform not in ("linux", "windows", "macos", "win32", "darwin"):
        raise AuditInputError(f"unsupported platform: {platform}")
    enabled_gtk = platform == "linux" if gtk is None else gtk
    root = _absolute(source_root)
    audits = inventory(root, gtk=enabled_gtk)
    try:
        result = run_audits(root, gtk=enabled_gtk)
    except (AuditInputError, OSError) as exc:
        result = AuditResult(errors=(Finding("completion", "audit_input", root.as_posix(), str(exc)),))
    failed = sorted({finding.audit for finding in result.errors})
    advisory = sorted({finding.audit for finding in result.warnings})
    report_path = output_dir / "guardrails.json"
    output_dir.mkdir(parents=True, exist_ok=True)
    report = {
        "schema_version": 1,
        "source_root": root.as_posix(),
        "platform": platform,
        "inventory": [
            {"name": audit.name, "mode": audit.mode, "condition": audit.condition, "enabled": audit in audits}
            for audit in inventory(root)
        ],
        "failed": failed,
        "advisory": advisory,
        "errors": [asdict(finding) for finding in result.errors],
        "warnings": [asdict(finding) for finding in result.warnings],
    }
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return CompletionResult(result.errors, result.warnings, report_path)
