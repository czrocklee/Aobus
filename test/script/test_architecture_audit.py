"""Tests for the declarative application architecture audit.

The policy oracle is the shared Python backend (ao.core.guardrails); these
cases prove rule adjudication over a governed fixture tree, the generic
forbidden-include adapter, and UIModel frontend neutrality.
"""

import tempfile
import unittest
from pathlib import Path

from ao.core import guardrails


class ArchitectureAuditTest(unittest.TestCase):
    def test_rule_examples_are_adjudicated_by_the_self_test(self):
        # The self-test proves every registered rule classifies its published
        # rejected/allowed sample pair; it fails closed on coverage drift.
        guardrails.architecture_self_test()

    def test_audit_reports_all_violations_from_a_governed_source_tree(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            source_root = Path(temp_dir)
            for relative_dir in (
                "app/include/ao/desktop",
                "app/include/ao/i18n",
                "app/include/ao/rt",
                "app/include/ao/systemmedia",
                "app/include/ao/uimodel",
                "app/uimodel",
                "app/desktop",
                "app/i18n",
                "app/runtime",
                "app/linux-gtk",
                "app/systemmedia",
                "app/macos-appkit",
                "app/windows-winui",
                "app/tui",
                "app/cli",
                "include/ao/yaml",
                "lib",
                "test",
            ):
                (source_root / relative_dir).mkdir(parents=True, exist_ok=True)

            for relative_file in (
                "app/include/ao/rt/TrackField.h",
                "app/include/ao/rt/TrackPresentation.h",
                "app/include/ao/rt/completion/CompletionItem.h",
                "include/ao/audio/BackendProvider.h",
            ):
                path = source_root / relative_file
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()

            violation = source_root / "app/tui/Violation.cpp"
            violation.write_text("#include <ao/rt/CoreRuntime.h>\n", encoding="utf-8")
            violation.with_suffix(".mm").write_text("#include <ao/rt/CoreRuntime.h>\n", encoding="utf-8")
            (source_root / "app/systemmedia/Gui.cpp").write_text("#include <gtkmm/widget.h>\n", encoding="utf-8")
            (source_root / "app/systemmedia/Native.cpp").write_text("#include <giomm/file.h>\n", encoding="utf-8")
            (source_root / "app/systemmedia/Umbrella.cpp").write_text("#include <gtkmm.h>\n", encoding="utf-8")
            (source_root / "app/systemmedia/GdkUmbrella.mm").write_text('#import "gdkmm.h"\n', encoding="utf-8")
            (source_root / "app/systemmedia/TuiComponent.cpp").write_text(
                "#include <ftxui/component/component.hpp>\n", encoding="utf-8"
            )
            (source_root / "app/systemmedia/TuiDom.mm").write_text(
                '#import "ftxui/dom/elements.hpp"\n', encoding="utf-8"
            )
            (source_root / "app/systemmedia/Documented.h").write_text(
                "// #include <gtkmm.h>\n// #include <ftxui/component/component.hpp>\n"
                '/*\n#include <gdkmm.h>\n#include <gtk/gtk.h>\n#import "ftxui/dom/elements.hpp"\n*/\n'
                "#include <giomm.h>\n#include <glibmm.h>\n",
                encoding="utf-8",
            )
            (source_root / "app/systemmedia/WinUi.cpp").write_text(
                "#include <winrt/Microsoft.UI.Dispatching.h>\n", encoding="utf-8"
            )
            (source_root / "app/include/ao/systemmedia/Frontend.h").write_text(
                '#include "tui/App.h"\n', encoding="utf-8"
            )
            (source_root / "app/systemmedia/Os.cpp").write_text(
                "#include <winrt/Windows.Media.h>\n#import <MediaPlayer/MediaPlayer.h>\n", encoding="utf-8"
            )
            (source_root / "app/runtime/Reverse.cpp").write_text(
                '#include "systemmedia/linux/MprisBusSession.h"\n', encoding="utf-8"
            )
            (source_root / "app/cli/Reverse.cpp").write_text(
                '#include "ao/systemmedia/linux/MprisBridge.h"\n', encoding="utf-8"
            )
            (source_root / "app/include/ao/uimodel/Reverse.h").write_text(
                "#include <ao/systemmedia/macos/MediaPlayerAdapter.h>\n", encoding="utf-8"
            )
            managed_state_violation = source_root / "app/tui/ManagedState.def"
            managed_state_violation.write_text("#include <ao/yaml/Reflect.h>\n", encoding="utf-8")
            suffix_violation = source_root / "app/tui/Unsupported.cc"
            suffix_violation.write_text("// unsupported suffix\n", encoding="utf-8")
            suffix_violation.with_suffix(".m").write_text("// unsupported suffix\n", encoding="utf-8")
            (source_root / "app/uimodel/Native.mm").write_text("#import <AppKit/AppKit.h>\n", encoding="utf-8")
            (source_root / "app/uimodel/NativeModule.h").write_text("@import Foundation.NSString;\n", encoding="utf-8")
            (source_root / "app/uimodel/Documented.h").write_text(
                "// @import AppKit;\n/*\n@import Foundation;\n*/\n", encoding="utf-8"
            )
            core_value_includes = (
                "#include <ao/library/RecordingDate.h>\n"
                '#include "ao/library/RecordingDate.h"\n'
                "#include <ao/library/Credits.h>\n"
                '#include "ao/library/Credits.h"\n'
            )
            for relative_file in ("app/include/ao/uimodel/CoreValues.h", "app/uimodel/CoreValues.cpp"):
                (source_root / relative_file).write_text(core_value_includes, encoding="utf-8")

            uimodel_core_rejections = {
                "MusicLibrary": "#include <ao/library/MusicLibrary.h>\n",
                "TrackView": '#include "ao/library/TrackView.h"\n',
                "TrackBuilder": '#include "ao/library/TrackBuilder.h"\n',
                "ListStore": '#include "ao/library/ListStore.h"\n',
                "ResourceStore": '#include "ao/library/ResourceStore.h"\n',
                "DictionaryStore": '#include "ao/library/DictionaryStore.h"\n',
                "FileManifestStore": '#include "ao/library/FileManifestStore.h"\n',
                "DateSuffix": "#include <ao/library/RecordingDate.hpp>\n",
                "DateExtension": '#include "ao/library/RecordingDate.h.extra"\n',
                "DateTraversal": '#include "ao/library/RecordingDate.h/../MusicLibrary.h"\n',
                "RetiredMusicians": "#include <ao/library/Musicians.h>\n",
                "CreditsPrefix": '#include "ao/library/CreditsExtra.h"\n',
                "CreditsChild": '#include "ao/library/Credits.h/TrackView.h"\n',
                "CreditsDetail": '#include "ao/library/detail/Credits.h"\n',
                "LibraryTraversal": '#include "ao/library/../lmdb/Transaction.h"\n',
                "Lmdb": '#include "ao/lmdb/Transaction.h"\n',
                "Player": "#include <ao/audio/Player.h>\n",
                "Backend": '#include "ao/audio/Backend.h"\n',
                "Engine": "#include <ao/audio/Engine.h>\n",
                "AudioBackend": '#include "ao/audio/backend/Backend.h"\n',
                "AudioDetail": "#include <ao/audio/detail/Control.h>\n",
                "DateDelimiter": '#include <ao/library/RecordingDate.h"\n',
                "CreditsDelimiter": '#include "ao/library/Credits.h>\n',
                "WriteTransaction": "ao::library::WriteTransaction& transaction;\n",
                "WritableLibrary": "ao::library::WritableMusicLibrary& library;\n",
            }
            # Each file starts with all four allowed forms: filtering them must
            # not swallow a later forbidden include or write-authority reference.
            for name, forbidden_source in uimodel_core_rejections.items():
                (source_root / f"app/uimodel/CoreValuesThen{name}.cpp").write_text(
                    core_value_includes + "\n" + forbidden_source, encoding="utf-8"
                )

            (source_root / "app/macos-appkit/Violation.mm").write_text(
                "#include <ao/rt/CoreRuntime.h>\n#include <ao/yaml/Reflect.h>\n"
                'PlaybackTransport* transport;\nauto database = "data.mdb";\n',
                encoding="utf-8",
            )

            result = guardrails.audit_architecture_rules(source_root)

        findings = {(finding.rule, finding.path) for finding in result.errors}
        self.assertEqual(len(result.errors), 21 + len(uimodel_core_rejections))
        for relative_file in ("app/include/ao/uimodel/CoreValues.h", "app/uimodel/CoreValues.cpp"):
            self.assertNotIn(relative_file, {path for _, path in findings})
        for name in uimodel_core_rejections:
            self.assertIn(("uimodel_core", f"app/uimodel/CoreValuesThen{name}.cpp"), findings)
        for relative in (
            "app/systemmedia/Gui.cpp",
            "app/systemmedia/Umbrella.cpp",
            "app/systemmedia/GdkUmbrella.mm",
            "app/systemmedia/TuiComponent.cpp",
            "app/systemmedia/TuiDom.mm",
            "app/systemmedia/WinUi.cpp",
            "app/include/ao/systemmedia/Frontend.h",
        ):
            self.assertIn(("system_media_frontend", relative), findings)
        self.assertNotIn(("system_media_frontend", "app/systemmedia/Native.cpp"), findings)
        self.assertNotIn(("system_media_frontend", "app/systemmedia/Documented.h"), findings)
        self.assertNotIn(("system_media_frontend", "app/systemmedia/Os.cpp"), findings)
        for reverse in ("app/runtime/Reverse.cpp", "app/cli/Reverse.cpp", "app/include/ao/uimodel/Reverse.h"):
            self.assertIn(("system_media_reverse", reverse), findings)
        self.assertIn(("frontend_core", "app/tui/Violation.cpp"), findings)
        self.assertIn(("frontend_core", "app/tui/Violation.mm"), findings)
        self.assertIn(("managed_state_mechanism", "app/tui/ManagedState.def"), findings)
        self.assertIn(("unsupported_cpp_suffix", "app/tui/Unsupported.cc"), findings)
        self.assertIn(("unsupported_cpp_suffix", "app/tui/Unsupported.m"), findings)
        self.assertIn(("uimodel_platform", "app/uimodel/Native.mm"), findings)
        self.assertIn(("uimodel_platform", "app/uimodel/NativeModule.h"), findings)
        self.assertNotIn(("uimodel_platform", "app/uimodel/Documented.h"), findings)
        for rule in ("frontend_core", "managed_state_mechanism", "playback_internal", "frontend_library_path"):
            self.assertIn((rule, "app/macos-appkit/Violation.mm"), findings)
        # The remediation guidance for unsupported suffixes stays observable.
        self.assertTrue(any("use .cpp, .mm, .h, .hpp, or .def" in finding.detail for finding in result.errors))


class ForbiddenIncludeGuardrailTest(unittest.TestCase):
    def test_guardrail_checks_objective_cpp_sources(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "Boundary.mm").write_text("#include <forbidden/Api.h>\n", encoding="utf-8")
            result = guardrails.scan_forbidden([root], forbidden="forbidden/Api.h")
        self.assertFalse(result.ok)
        self.assertIn("Forbidden dependency in", result.errors[0].rule)
        self.assertIn("Boundary.mm", result.errors[0].path)

    def test_default_forbidden_include_regex_rejects_gtk_and_allows_shared_headers(self):
        for directive, include, rejected in (
            (directive, include, rejected)
            for directive in ("include", "import")
            for include, rejected in (("gtkmm/widget.h", True), ("ao/uimodel/Track.h", False))
        ):
            with self.subTest(directive=directive, include=include), tempfile.TemporaryDirectory() as temp_dir:
                root = Path(temp_dir)
                (root / "Boundary.mm").write_text(f"#{directive} <{include}>\n", encoding="utf-8")
                result = guardrails.scan_forbidden([root])
            self.assertEqual(not result.ok, rejected, result.errors)
            if rejected:
                self.assertIn("Forbidden dependency in", result.errors[0].rule)


class UimodelFrontendNeutralityTest(unittest.TestCase):
    def run_fixture(self, filename: str, source: str, *, create_source_root: bool = True):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        public_root = root / "public"
        source_root = root / "source"
        test_root = root / "test"
        public_root.mkdir()
        if create_source_root:
            source_root.mkdir()
        test_root.mkdir()
        (public_root / filename).write_text(source, encoding="utf-8")
        return guardrails.audit_uimodel("uimodel-neutrality", public_root, source_root, test_root)

    def test_neutrality_rejects_native_types_and_names(self):
        vocabulary_rejected = self.run_fixture("Projection.h", "ftxui::Event event;\n")
        self.assertFalse(vocabulary_rejected.ok, vocabulary_rejected.errors)
        self.assertIn("Projection.h", vocabulary_rejected.errors[0].path)
        self.assertIn("frontend's vocabulary", vocabulary_rejected.errors[0].detail)

        name_rejected = self.run_fixture("Tui.h", "struct SharedValue {};\n")
        self.assertFalse(name_rejected.ok, name_rejected.errors)
        self.assertIn("Tui.h", name_rejected.errors[0].path)
        self.assertIn("file names a frontend", name_rejected.errors[0].detail)

        for filename, source in (
            ("Projection.mm", "NSView* view;\n"),
            ("Projection.h", "NSWindow* window;\n"),
            ("Projection.h", "#import <AppKit/AppKit.h>\n"),
            ("Projection.mm", "#include <Cocoa/Cocoa.h>\n"),
            ("Projection.mm", "#import <Foundation/Foundation.h>\n"),
            ("Projection.h", "#include <CoreGraphics/CGGeometry.h>\n"),
            ("Projection.h", "NSButton* button;\n"),
            ("Projection.h", "NSString* title;\n"),
            ("Projection.h", "CGRect rect;\n"),
            ("Projection.h", "CGContextRef context;\n"),
            ("Projection.h", "NS_ENUM(int, Mode) { Default };\n"),
            ("Projection.h", "NS_OPTIONS(int, Options) { None };\n"),
            ("Projection.h", "NS_ASSUME_NONNULL_BEGIN\n"),
            ("Projection.h", "CG_EXTERN void configure();\n"),
            ("Projection.h", "CG_INLINE void configure() {}\n"),
            ("Projection.h", "auto colorSpace = kCGColorSpaceSRGB;\n"),
            ("Projection.mm", "@import AppKit;\n"),
            ("Projection.mm", "@import Cocoa;\n"),
            ("Projection.mm", "@import Foundation.NSString;\n"),
            ("Projection.mm", "@import CoreGraphics.CGGeometry;\n"),
            ("Projection.mm", "@import\nFoundation;\n"),
            ("Projection.mm", "@ import Foundation . NSString;\n"),
            ("Projection.mm", "/*\ncomment\n*/ @import AppKit;\n"),
            ("AppKitProjection.h", "struct SharedValue {};\n"),
        ):
            with self.subTest(filename=filename, source=source):
                rejected = self.run_fixture(filename, source)
                self.assertFalse(rejected.ok, (filename, source, rejected.errors))
                self.assertIn(filename, rejected.errors[0].path)

        native_comment_allowed = self.run_fixture(
            "Projection.mm",
            "// NSView and NSWindow remain frontend-owned.\n"
            "// @import AppKit;\n"
            "/*\n@import Foundation;\nNSString* title;\n*/\n"
            "struct SharedNSStringState {};\n"
            "@import FoundationExtras;\n"
            "@import Shared.AppKit;\n",
        )
        self.assertTrue(native_comment_allowed.ok, native_comment_allowed.errors)

        comment_allowed = self.run_fixture("Projection.h", "// ftxui::Event remains frontend-owned.\n")
        self.assertTrue(comment_allowed.ok, comment_allowed.errors)


if __name__ == "__main__":
    unittest.main()
