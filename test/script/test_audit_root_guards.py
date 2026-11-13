"""Root admission and content contracts of the frontend audit kinds.

The policy oracle is the shared Python backend (ao.core.guardrails); these
cases prove each frontend kind's root admission and that an accepted root is
actually scanned for its published violation classes.
"""

import tempfile
import unittest
from pathlib import Path

from ao.core import guardrails


class FrontendContentTest(unittest.TestCase):
    """One real content violation per rule proves an accepted root is scanned."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aobus frontend audit ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()

    def put(self, files):
        for relative, source in files.items():
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source, encoding="utf-8")

    def assert_finding(self, kind, files, path_fragment, message):
        self.put(files)
        result = guardrails.audit_frontend(kind, self.root)
        self.assertFalse(result.ok, (kind, result.errors))
        self.assertIn(path_fragment, result.errors[0].path)
        self.assertIn(message, result.errors[0].detail)

    def test_leaf_kinds_reject_a_high_authority_reach(self):
        for kind in ("gtk-leaf", "winui-leaf"):
            with self.subTest(kind=kind):
                self.assert_finding(
                    kind,
                    {"app/Leaf.h": "AppRuntime* reach;\n"},
                    "app/Leaf.h",
                    "reaches a high-authority dependency",
                )

    def test_gtk_leaf_scans_the_whole_frontend(self):
        rejected = {
            "layout/component/track/Leaf.cpp": "void build(ao::rt::AppRuntime& runtime);\n",
            "track/TrackPageHost.h": "void host(ao::rt::AppRuntime& runtime);\n",
            # Code sharing a line with the terminator that ends a doc comment is code.
            "track/Leaf.h": "/**\n * doc\n */ void bind(ao::rt::AppRuntime& runtime);\n",
            # A registrations-shaped filename outside the named roots is not exempt.
            "track/TrackToolbarRegistrations.cpp": "void registerAll(ao::rt::AppRuntime& runtime);\n",
        }
        for relative in rejected:
            with self.subTest(relative=relative):
                self.put({relative: rejected[relative]})
                result = guardrails.audit_frontend("gtk-leaf", self.root)
                self.assertFalse(result.ok, result.errors)
                self.assertIn(relative, result.errors[0].path)
                (self.root / relative).unlink()

        self.put(
            {
                "app/MainWindow.cpp": "void compose(ao::rt::AppRuntime& runtime);\n",
                # Prose naming the rule is not a reach for the type it names.
                "track/Documented.h": (
                    "/**\n"
                    " * Documented takes exact capabilities rather than the whole AppRuntime.\n"
                    " */\n"
                    "class Documented;\n"
                    "/* AppRuntime stays at the composition root. */\n"
                    "void configure(ao::rt::ViewService& views); /* not the AppRuntime */\n"
                    # A block comment need not mark its continuation lines.
                    "/*\nAppRuntime is composed by the window.\n*/\n"
                ),
                "layout/runtime/LayoutRuntime.h": "class LayoutRuntime;\n",
                "layout/component/track/TrackRegistrations.cpp": "void registerAll(ao::rt::AppRuntime& runtime);\n",
                "list/ListNavigationController.cpp": "void build(ao::rt::AppRuntime& runtime);\n",
                "track/TrackPageHost.h": "void host(ao::rt::PlaybackService& playback);\n",
                "layout/component/track/Leaf.cpp": "void build(ao::rt::PlaybackService& playback);\n",
            }
        )
        result = guardrails.audit_frontend("gtk-leaf", self.root)
        self.assertTrue(result.ok, result.errors)

    def test_winui_leaf_scans_the_whole_frontend(self):
        rejected = {
            "playback/TransportButton.cpp": "void build(ao::winui::LibrarySession& session);\n",
            "platform/MediaBridge.h": "void bind(ao::rt::AppRuntime& runtime);\n",
            "platform/Leaf.h": "/**\n * doc\n */ void bind(ao::winui::LibrarySession& session);\n",
        }
        for relative in rejected:
            with self.subTest(relative=relative):
                self.put({relative: rejected[relative]})
                result = guardrails.audit_frontend("winui-leaf", self.root)
                self.assertFalse(result.ok, result.errors)
                self.assertIn(relative, result.errors[0].path)
                (self.root / relative).unlink()

        # track/ holds a named root, so its other files must still be governed.
        self.put(
            {
                "track/MainWindowTrack.cpp": "void compose(ao::rt::AppRuntime& runtime);\n",
                "track/TrackListController.cpp": "void drive(ao::rt::AppRuntime& runtime);\n",
            }
        )
        result = guardrails.audit_frontend("winui-leaf", self.root)
        self.assertFalse(result.ok, result.errors)
        self.assertIn("track/TrackListController.cpp", result.errors[0].path)
        self.assertNotIn("track/MainWindowTrack.cpp", {finding.path for finding in result.errors})

        for relative in ("track/MainWindowTrack.cpp", "track/TrackListController.cpp"):
            (self.root / relative).unlink()
        self.put(
            {
                "MainWindow.xaml.cpp": "void compose(ao::rt::AppRuntime& runtime);\n",
                # Prose naming the rule is not a reach for the types it names.
                "platform/Documented.h": (
                    "/**\n"
                    " * Documented is built by the LibrarySession from AppRuntime services.\n"
                    " */\n"
                    "class Documented;\n"
                    # A block comment need not mark its continuation lines.
                    "/*\nLibrarySession composes this from AppRuntime.\n*/\n"
                ),
                "app/LibrarySession.h": "class LibrarySession;\n",
                "layout/ShellBuilder.cpp": "void build(ao::rt::AppRuntime& runtime);\n",
                "shell/MainWindowShell.cpp": "void compose(ao::winui::LibrarySession& session);\n",
                "platform/MediaBridge.h": "void bind(ao::rt::PlaybackService& playback);\n",
                "playback/TransportButton.cpp": "void build(ao::rt::PlaybackService& playback);\n",
            }
        )
        self.assertTrue(guardrails.audit_frontend("winui-leaf", self.root).ok)

    def test_winui_events_rejects_unrevoked_owner_registrations(self):
        rejected = {
            "playback/Transport.cpp": "  _button.Click({this, &Transport::onClicked});\n",
            "wrapped/Transport.cpp": (
                "  _button.Click(\n    {this, &Transport::onClicked});\n"
                "  _panel.SizeChanged(\n    [this](auto&&, auto&&) { relayout(); });\n"
            ),
            # A comment between the call and its argument does not hide the argument.
            "interleaved/Transport.cpp": (
                "  _panel.SizeChanged(\n"
                "    /* the panel outlives this owner */\n"
                "    [this](auto&&, auto&&) { relayout(); });\n"
            ),
            # A compound capture list reaches the owner exactly as [this] does.
            "compound/Transport.cpp": "  _panel.SizeChanged([=, this](auto&&, auto&&) { relayout(); });\n",
        }
        for relative in rejected:
            with self.subTest(relative=relative):
                self.put({relative: rejected[relative]})
                result = guardrails.audit_frontend("winui-events", self.root)
                self.assertFalse(result.ok, result.errors)
                self.assertIn("without auto_revoke", result.errors[0].detail)
                (self.root / relative).unlink()

        self.put(
            {
                "playback/Transport.cpp": (
                    "  _clickRevoker = _button.Click(winrt::auto_revoke, {this, &Transport::onClicked});\n"
                    "  _sizeRevoker = _panel.SizeChanged(\n"
                    "    winrt::auto_revoke,\n"
                    "    [this](auto&&, auto&&) { relayout(); });\n"
                    "  _timer.Tick([weak = get_weak()](auto&&, auto&&) { poll(); });\n"
                    "  // _button.Click({this, &Transport::onClicked}) would not revoke.\n"
                    "  /* _panel.SizeChanged(\n"
                    "     [this](auto&&, auto&&) { relayout(); }) would not revoke either. */\n"
                ),
                "playback/SelfRegistering.cpp": (
                    "  Loaded(\n    [this](auto&&, auto&&) { onLoaded(); });\n"
                    "  SizeChanged([this](auto&&, auto&&) { updateGeometry(); });\n"
                ),
            }
        )
        self.assertTrue(guardrails.audit_frontend("winui-events", self.root).ok)

    def test_winui_events_rejects_hand_managed_tokens_and_unsupported_weak_references(self):
        self.put({"playback/Transport.cpp": "  winrt::event_token _clickToken{};\n"})
        result = guardrails.audit_frontend("winui-events", self.root)
        self.assertFalse(result.ok, result.errors)
        self.assertIn("raw event_token", result.errors[0].detail)

        # Code sharing a line with the terminator that ends a doc comment is code.
        self.put({"playback/Transport.h": "/**\n * doc\n */ winrt::event_token _clickToken{};\n"})
        result = guardrails.audit_frontend("winui-events", self.root)
        self.assertFalse(result.ok, result.errors)
        self.assertIn("raw event_token", result.errors[0].detail)

        self.put({"app/Shell.cpp": "  _changed = AppWindow().Changed(winrt::auto_revoke, handler);\n"})
        result = guardrails.audit_frontend("winui-events", self.root)
        self.assertFalse(result.ok, result.errors)
        self.assertIn("does not support weak references", result.errors[0].detail)

    def test_winui_state_rejects_the_legacy_push_contract_and_raw_fan_out(self):
        for source, fragment in (
            ("onShellStateChanged();\n", "onShellStateChanged"),
            ("ctx.observers.push_back(observer);\n", "raw component fan-out"),
        ):
            with self.subTest(fragment=fragment):
                self.put({"app/Leaf.h": source})
                result = guardrails.audit_frontend("winui-state", self.root)
                self.assertFalse(result.ok, result.errors)
                self.assertIn(fragment, result.errors[0].detail)


if __name__ == "__main__":
    unittest.main()
