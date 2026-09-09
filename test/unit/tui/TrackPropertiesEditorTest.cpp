// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "tui/TrackPropertiesEditor.h"

#include "RenderTestSupport.h"
#include "TrackPropertiesEditorTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace ao::tui::test
{
  namespace
  {
    /// The rendered tab strip alone, so a page name is not confused with a row label.
    std::string tabStripLine(std::string const& text)
    {
      auto const index = lineIndexContaining(text, "Metadata");

      if (index < 0)
      {
        return {};
      }

      auto stream = std::istringstream{text};
      auto line = std::string{};

      for (auto remaining = index; remaining >= 0; --remaining)
      {
        std::getline(stream, line);
      }

      return line;
    }
  } // namespace

  TEST_CASE("TrackPropertiesEditor - opens a single target without mixed markers", "[tui][unit][editor]")
  {
    auto editor = makeEditor({TrackFixture{.title = "So What", .album = "Kind of Blue", .year = 1959}});

    CHECK(editor.targetCount() == 1);
    CHECK_FALSE(editor.isDirty());

    auto const text = frame(editor);

    CHECK(text.contains("Track Properties"));
    CHECK(text.contains("Kind of Blue"));
    // One target agrees with itself, so nothing is mixed and no batch language
    // belongs on the surface.
    CHECK_FALSE(text.contains("<Multiple Values>"));
    CHECK_FALSE(text.contains("Applies to all"));
    // A single target has nothing to review, so the page is not offered. The
    // strip is checked rather than the frame, because "Total Tracks" is an
    // ordinary metadata row label.
    auto const stripLine = tabStripLine(text);
    REQUIRE_FALSE(stripLine.empty());
    CHECK_FALSE(stripLine.contains("Tracks"));

    editor.tryHandleEvent(ftxui::Event::Tab);
    CHECK(editor.tab() == TrackEditorTab::Tags);
    editor.tryHandleEvent(ftxui::Event::Tab);
    CHECK(editor.tab() == TrackEditorTab::Properties);

    editor.tryHandleEvent(ftxui::Event::TabReverse);
    CHECK(editor.tab() == TrackEditorTab::Tags);
    editor.tryHandleEvent(ftxui::Event::TabReverse);
    CHECK(editor.tab() == TrackEditorTab::Metadata);
    editor.tryHandleEvent(ftxui::Event::TabReverse);
    CHECK(editor.tab() == TrackEditorTab::Properties);
  }

  TEST_CASE("TrackPropertiesEditor - shows a batch as counts, not the first target's values", "[tui][unit][editor]")
  {
    auto editor = makeEditor({
      TrackFixture{.title = "So What", .album = "Kind of Blue", .year = 1959},
      TrackFixture{.title = "Blue in Green", .album = "Kind of Blue", .year = 1959},
      TrackFixture{.title = "Flamenco Sketches", .album = "Kind of Blue", .year = 1960},
    });

    auto const text = frame(editor);

    CHECK(editor.targetCount() == 3);
    CHECK(text.contains("Edit 3 tracks"));
    CHECK(text.contains("Changes apply to all 3 tracks"));
    // Album agrees across the batch, so it shows its real value; Title and Year
    // disagree, and neither may be shown as though it spoke for the selection.
    CHECK(text.contains("Kind of Blue"));
    CHECK_FALSE(text.contains("So What"));
    CHECK(text.contains("<Multiple Values>"));
  }

  TEST_CASE("TrackPropertiesEditor - grants Apply intent only for an accepted edit", "[tui][unit][editor]")
  {
    auto editor = makeEditor({
      TrackFixture{.title = "So What", .album = "Kind of Blue", .year = 1959},
      TrackFixture{.title = "Blue in Green", .album = "Kind of Blue", .year = 1959},
    });

    SECTION("Walking the whole form changes nothing")
    {
      for (std::int32_t step = 0; step < 40; ++step)
      {
        editor.tryHandleEvent(ftxui::Event::ArrowDown);
      }

      for (std::int32_t step = 0; step < 10; ++step)
      {
        editor.tryHandleEvent(ftxui::Event::ArrowUp);
        editor.tryHandleEvent(ftxui::Event::ArrowLeft);
        editor.tryHandleEvent(ftxui::Event::ArrowRight);
      }

      CHECK_FALSE(editor.isDirty());
      CHECK(frame(editor).contains("Album"));
    }

    SECTION("Typing into a field includes it")
    {
      focusRow(editor, "Album");
      typeText(editor, "!");

      CHECK(editor.isDirty());
      CHECK(frame(editor).contains("* Album"));
    }

    SECTION("A rejected paste is not an edit")
    {
      focusRow(editor, "Album");
      editor.tryHandleEvent(ftxui::Event::Character("\n"));

      CHECK_FALSE(editor.isDirty());
      CHECK_FALSE(frame(editor).contains("* Album"));
    }

    SECTION("Backspace with nothing behind the cursor is not an edit")
    {
      focusRow(editor, "Title");
      editor.tryHandleEvent(ftxui::Event::Backspace);

      // Title is mixed, so its input starts empty and Backspace has no text to
      // remove. Treating that as an edit would arm a clear the user never asked
      // for.
      CHECK_FALSE(editor.isDirty());
    }
  }

  TEST_CASE("TrackPropertiesEditor - Ctrl-U on a mixed field arms an explicit clear", "[tui][unit][editor]")
  {
    auto editor = makeEditor({
      TrackFixture{.title = "So What", .album = "Kind of Blue", .year = 1959},
      TrackFixture{.title = "Blue in Green", .album = "Kind of Blue", .year = 1959},
    });

    focusRow(editor, "Title");
    editor.tryHandleEvent(clearEvent());

    auto const text = frame(editor);

    CHECK(editor.isDirty());
    CHECK(text.contains("* Title"));
    // The count is the whole point: an unlabelled empty row would read as a
    // reveal control rather than as a clear applied to every target.
    CHECK(text.contains("Clear for all 2 tracks"));

    SECTION("Typing replaces the clear rather than appending to a marker")
    {
      typeText(editor, "New");

      auto const typed = frame(editor);
      CHECK(typed.contains("New"));
      CHECK_FALSE(typed.contains("Clear for all 2 tracks"));
    }

    SECTION("Restoring returns the row to the aggregate baseline")
    {
      typeText(editor, "New");
      editor.tryHandleEvent(restoreEvent());

      auto const reset = frame(editor);
      CHECK_FALSE(editor.isDirty());
      CHECK_FALSE(reset.contains("* Title"));
      CHECK_FALSE(reset.contains("New"));
      CHECK(reset.contains("<Multiple Values>"));
    }
  }

  TEST_CASE("TrackPropertiesEditor - Ctrl-U on a common field clears it, and Ctrl-G restores it", "[tui][unit][editor]")
  {
    auto editor = makeEditor({
      TrackFixture{.title = "So What", .album = "Kind of Blue", .year = 1959},
      TrackFixture{.title = "Blue in Green", .album = "Kind of Blue", .year = 1959},
    });

    focusRow(editor, "Album");
    editor.tryHandleEvent(clearEvent());

    auto const text = frame(editor);

    CHECK(editor.isDirty());
    CHECK(text.contains("* Album"));
    CHECK(text.contains("Clear for all 2 tracks"));

    editor.tryHandleEvent(restoreEvent());
    auto const restored = frame(editor);
    CHECK_FALSE(editor.isDirty());
    CHECK_FALSE(restored.contains("* Album"));
    CHECK(restored.contains("Kind of Blue"));
  }

  TEST_CASE("TrackPropertiesEditor - reports an included number it cannot parse", "[tui][unit][editor]")
  {
    auto editor = makeEditor({TrackFixture{.title = "So What", .album = "Kind of Blue", .year = 1959}});

    focusRow(editor, "Year");
    editor.tryHandleEvent(ftxui::Event::End);
    editor.tryHandleEvent(ftxui::Event::Backspace);
    typeText(editor, "0x");

    CHECK(editor.isDirty());
    CHECK(frame(editor).contains("Whole number required"));

    SECTION("Removing the bad character clears the error and keeps the intent")
    {
      editor.tryHandleEvent(ftxui::Event::Backspace);

      CHECK(editor.isDirty());
      CHECK_FALSE(frame(editor).contains("Whole number required"));
    }
  }

  TEST_CASE("TrackPropertiesEditor - activates a page as the tab strip moves", "[tui][unit][editor]")
  {
    auto editor = makeEditor({
      TrackFixture{.title = "So What", .album = "Kind of Blue", .year = 1959},
      TrackFixture{.title = "Blue in Green", .album = "Kind of Blue", .year = 1959},
    });

    REQUIRE(editor.tab() == TrackEditorTab::Metadata);

    editor.tryHandleEvent(ftxui::Event::Tab);
    CHECK(editor.tab() == TrackEditorTab::Tags);

    editor.tryHandleEvent(ftxui::Event::Tab);
    CHECK(editor.tab() == TrackEditorTab::Properties);

    editor.tryHandleEvent(ftxui::Event::Tab);
    CHECK(editor.tab() == TrackEditorTab::Tracks);

    auto const text = frame(editor);
    // The Tracks page reviews the captured batch by name and path so the user
    // can see exactly what a save would reach.
    CHECK(text.contains("So What"));
    CHECK(text.contains("Blue in Green"));
    CHECK(text.contains("/music/So What.flac"));

    editor.tryHandleEvent(ftxui::Event::Tab);
    CHECK(editor.tab() == TrackEditorTab::Metadata);

    for (auto const tab :
         {TrackEditorTab::Tracks, TrackEditorTab::Properties, TrackEditorTab::Tags, TrackEditorTab::Metadata})
    {
      editor.tryHandleEvent(ftxui::Event::TabReverse);
      CHECK(editor.tab() == tab);
    }
  }

  TEST_CASE("TrackPropertiesEditor - aggregates read-only properties without offering intent", "[tui][unit][editor]")
  {
    auto editor = makeEditor({
      TrackFixture{.title = "So What", .album = "Kind of Blue", .year = 1959, .codec = "FLAC"},
      TrackFixture{.title = "Blue in Green", .album = "Kind of Blue", .year = 1959, .codec = "ALAC"},
    });

    selectTab(editor, TrackEditorTab::Properties);
    REQUIRE(editor.tab() == TrackEditorTab::Properties);

    auto const text = frame(editor);

    // Read-only rows follow the same aggregate policy as editable ones, so a
    // disagreeing codec is reported as mixed rather than as the first target's.
    CHECK(text.contains("Codec"));
    CHECK(text.contains("<Multiple Values>"));
    CHECK_FALSE(text.contains("FLAC"));

    editor.tryHandleEvent(ftxui::Event::Character(' '));
    CHECK_FALSE(editor.isDirty());
  }

  TEST_CASE("TrackPropertiesEditor - asks before dropping a draft", "[tui][unit][editor]")
  {
    auto editor = makeEditor({TrackFixture{.title = "So What", .album = "Kind of Blue", .year = 1959}});

    SECTION("A clean editor closes at once")
    {
      editor.tryHandleEvent(ftxui::Event::Escape);

      CHECK(editor.takeRequest() == TrackEditorRequest::Close);
      CHECK_FALSE(editor.isConfirmingDiscard());
    }

    SECTION("A dirty editor asks first and survives a declined discard")
    {
      focusRow(editor, "Album");
      typeText(editor, "!");
      REQUIRE(editor.isDirty());

      editor.tryHandleEvent(ftxui::Event::Escape);
      CHECK(editor.isConfirmingDiscard());
      CHECK(editor.takeRequest() == TrackEditorRequest::None);
      CHECK(frame(editor).contains("Discard changes?"));

      editor.tryHandleEvent(ftxui::Event::Escape);
      CHECK_FALSE(editor.isConfirmingDiscard());
      CHECK(editor.takeRequest() == TrackEditorRequest::None);
      CHECK(editor.isDirty());

      editor.tryHandleEvent(ftxui::Event::Escape);
      editor.tryHandleEvent(ftxui::Event::Return);
      CHECK(editor.takeRequest() == TrackEditorRequest::Close);
    }
  }

  TEST_CASE("TrackPropertiesEditor - consumes every key it does not use", "[tui][unit][editor]")
  {
    auto editor = makeEditor({TrackFixture{.title = "So What", .album = "Kind of Blue", .year = 1959}});
    selectTab(editor, TrackEditorTab::Properties);

    // Nothing may fall through to workspace dispatch: the workspace is not on
    // screen, so a command run against it would be invisible.
    CHECK(editor.tryHandleEvent(ftxui::Event::Character('q')));
    CHECK(editor.tryHandleEvent(ftxui::Event::F5));
    CHECK(editor.tryHandleEvent(ftxui::Event::Custom));
    CHECK(editor.takeRequest() == TrackEditorRequest::None);
  }

  TEST_CASE("TrackPropertiesEditor - fits an 80x24 terminal", "[tui][unit][editor]")
  {
    auto editor = makeEditor({
      TrackFixture{.title = "So What", .album = "Kind of Blue", .year = 1959},
      TrackFixture{.title = "Blue in Green", .album = "Kind of Blue", .year = 1959},
    });

    auto const rendered = renderElement(editor.render(), kTerminalColumns, kTerminalRows);

    CHECK(rendered.screen.dimx() == kTerminalColumns);
    CHECK(rendered.screen.dimy() == kTerminalRows);
    // The header, tab strip, footer, and Apply action all have to survive the
    // smallest supported terminal; a body that squeezed them out would hide the
    // only way to leave the editor.
    CHECK(rendered.text.contains("Edit 2 tracks"));
    CHECK(rendered.text.contains("Metadata"));
    CHECK(rendered.text.contains("Apply to 2 tracks"));
    CHECK(rendered.text.contains("Esc"));
  }

  TEST_CASE("TrackPropertiesEditor - offers Apply only for a valid included draft", "[tui][unit][editor]")
  {
    auto editor = makeEditor({
      TrackFixture{.title = "So What", .album = "Kind of Blue", .year = 1959},
      TrackFixture{.title = "Blue in Green", .album = "Kind of Blue", .year = 1959},
    });

    SECTION("An untouched draft has nothing to write")
    {
      CHECK_FALSE(editor.canApply());

      editor.tryHandleEvent(applyEvent());
      CHECK(editor.takeRequest() == TrackEditorRequest::None);
    }

    SECTION("An edited field can be applied")
    {
      focusRow(editor, "Album");
      typeText(editor, " 2");

      REQUIRE(editor.canApply());
      CHECK(editor.patchSummary() == TrackEditorPatchSummary{.fieldCount = 1, .clearCount = 0});

      editor.tryHandleEvent(applyEvent());
      CHECK(editor.takeRequest() == TrackEditorRequest::Apply);
      // The request is taken once; a second read must not resubmit it.
      CHECK(editor.takeRequest() == TrackEditorRequest::None);
    }

    SECTION("An invalid number blocks the whole draft")
    {
      focusRow(editor, "Year");
      editor.tryHandleEvent(ftxui::Event::End);
      typeText(editor, "x");

      REQUIRE(editor.isDirty());
      CHECK_FALSE(editor.canApply());

      editor.tryHandleEvent(applyEvent());
      CHECK(editor.takeRequest() == TrackEditorRequest::None);
    }

    SECTION("An explicit clear counts as a pending clear")
    {
      focusRow(editor, "Title");
      editor.tryHandleEvent(clearEvent());

      CHECK(editor.patchSummary() == TrackEditorPatchSummary{.fieldCount = 1, .clearCount = 1});
      CHECK(frame(editor).contains("1 clear"));
    }
  }

  TEST_CASE("TrackPropertiesEditor - builds a patch from edited fields only", "[tui][unit][editor]")
  {
    auto editor = makeEditor({
      TrackFixture{.title = "So What", .album = "Kind of Blue", .year = 1959},
      TrackFixture{.title = "Blue in Green", .album = "Kind of Blue", .year = 1961},
    });

    SECTION("An untouched draft writes nothing")
    {
      auto const patch = editor.buildPatch();

      CHECK_FALSE(patch.metadata.optTitle);
      CHECK_FALSE(patch.metadata.optAlbum);
      CHECK_FALSE(patch.metadata.optYear);
    }

    SECTION("A replaced common field is written and its neighbours are left alone")
    {
      focusRow(editor, "Album");
      editor.tryHandleEvent(ftxui::Event::End);
      typeText(editor, " (Legacy)");

      auto const patch = editor.buildPatch();

      REQUIRE(patch.metadata.optAlbum);
      CHECK(*patch.metadata.optAlbum == "Kind of Blue (Legacy)");
      CHECK_FALSE(patch.metadata.optTitle);
      CHECK_FALSE(patch.metadata.optYear);
    }

    SECTION("A replaced mixed field converges every target")
    {
      focusRow(editor, "Title");
      typeText(editor, "Flamenco Sketches");

      auto const patch = editor.buildPatch();

      REQUIRE(patch.metadata.optTitle);
      CHECK(*patch.metadata.optTitle == "Flamenco Sketches");
      CHECK_FALSE(patch.metadata.optYear);
    }

    SECTION("A cleared mixed number writes the codec's clear value")
    {
      focusRow(editor, "Year");
      editor.tryHandleEvent(clearEvent());

      auto const patch = editor.buildPatch();

      REQUIRE(patch.metadata.optYear);
      CHECK(*patch.metadata.optYear == 0);
      CHECK_FALSE(patch.metadata.optTitle);
    }
  }

  TEST_CASE("TrackPropertiesEditor - keeps the draft when a write cannot proceed", "[tui][unit][editor]")
  {
    auto editor = makeEditor({TrackFixture{.title = "So What", .album = "Kind of Blue", .year = 1959}});

    focusRow(editor, "Album");
    editor.tryHandleEvent(ftxui::Event::End);
    typeText(editor, "!");
    REQUIRE(editor.canApply());

    SECTION("A submission in flight consumes every key")
    {
      editor.setStatus(TrackEditorStatus::Submitting);

      CHECK(editor.tryHandleEvent(applyEvent()));
      CHECK(editor.tryHandleEvent(ftxui::Event::Escape));
      CHECK(editor.tryHandleEvent(reloadEvent()));
      CHECK(editor.takeRequest() == TrackEditorRequest::None);
      CHECK(frame(editor).contains("Applying"));
    }

    SECTION("A stale session keeps the draft and offers reload or close")
    {
      editor.setStatus(TrackEditorStatus::Stale);

      CHECK_FALSE(editor.canApply());
      CHECK(editor.isDirty());
      CHECK(editor.buildPatch().metadata.optAlbum);

      auto const text = frame(editor);
      CHECK(text.contains("library changed"));
      CHECK(text.contains("reload"));
      CHECK_FALSE(text.contains("Apply to"));

      editor.tryHandleEvent(applyEvent());
      CHECK(editor.takeRequest() == TrackEditorRequest::None);
    }

    SECTION("A failed submission shows its own diagnostic")
    {
      editor.setStatus(TrackEditorStatus::Failed, "Library is busy");

      CHECK_FALSE(editor.canApply());
      CHECK(frame(editor).contains("Library is busy"));
    }

    SECTION("A retryable warning clears when editing resumes")
    {
      editor.setStatus(TrackEditorStatus::Ready, "Library is busy");
      REQUIRE(frame(editor).contains("Library is busy"));

      editor.tryHandleEvent(ftxui::Event::ArrowLeft);

      CHECK_FALSE(frame(editor).contains("Library is busy"));
      CHECK(frame(editor).contains("Ctrl-U"));
      CHECK(frame(editor).contains("Ctrl-G"));
      CHECK(editor.canApply());
      CHECK(editor.buildPatch().metadata.optAlbum == "Kind of Blue!");
    }
  }

  TEST_CASE("TrackPropertiesEditor - localized recovery shortcuts remain readable in short terminals",
            "[tui][regression][editor]")
  {
    for (auto const* const locale : {"de", "fr", "ja", "zh-Hans", "zh-Hant", "qps-ploc"})
    {
      auto editor = makeEditor({TrackFixture{.title = "Track", .album = "Blue"}}, {}, {}, {}, locale);
      editor.setStatus(TrackEditorStatus::Stale);

      auto const rendered = renderElement(editor.renderModal(80, 16), 80, 16);
      CHECK(rendered.text.contains("Ctrl-R"));
      CHECK(rendered.text.contains("Esc"));
    }
  }

  TEST_CASE("TrackPropertiesEditor - long confirmation prompts wrap without losing the final action",
            "[tui][regression][editor]")
  {
    auto editor = makeEditor({TrackFixture{.title = "Track", .album = "Blue"}}, {}, {}, {}, "de");
    typeText(editor, "!");
    editor.tryHandleEvent(reloadEvent());
    REQUIRE(editor.isConfirmingReload());

    auto const rendered = renderElement(editor.renderModal(80, 16), 80, 16);
    CHECK(rendered.text.contains("Eingabe"));
    CHECK(rendered.text.contains("Escape"));
    CHECK(rendered.text.contains("Weiterbearbeiten"));
  }

  TEST_CASE("TrackPropertiesEditor - treats reload as a destructive draft action", "[tui][unit][editor]")
  {
    auto editor = makeEditor({TrackFixture{.title = "So What", .album = "Kind of Blue", .year = 1959}});

    SECTION("A clean editor reloads without a question")
    {
      editor.tryHandleEvent(reloadEvent());

      CHECK_FALSE(editor.isConfirmingReload());
      CHECK(editor.takeRequest() == TrackEditorRequest::Reload);
    }

    SECTION("A dirty editor asks first and survives a declined reload")
    {
      focusRow(editor, "Album");
      typeText(editor, "!");
      REQUIRE(editor.isDirty());

      editor.tryHandleEvent(reloadEvent());
      CHECK(editor.isConfirmingReload());
      CHECK(editor.takeRequest() == TrackEditorRequest::None);
      CHECK(frame(editor).contains("Reload and lose changes?"));

      auto const optBeforePrompt = editor.buildPatch().metadata.optAlbum;
      typeText(editor, "ignored");
      editor.tryHandleEvent(ftxui::Event::Tab);
      editor.tryHandleEvent(clearEvent());
      editor.tryHandleEvent(applyEvent());
      CHECK(editor.isConfirmingReload());
      CHECK(editor.tab() == TrackEditorTab::Metadata);
      CHECK(editor.takeRequest() == TrackEditorRequest::None);
      CHECK(editor.buildPatch().metadata.optAlbum == optBeforePrompt);

      editor.tryHandleEvent(ftxui::Event::Escape);
      CHECK_FALSE(editor.isConfirmingReload());
      CHECK(editor.isDirty());

      editor.tryHandleEvent(reloadEvent());
      editor.tryHandleEvent(ftxui::Event::Return);
      CHECK(editor.takeRequest() == TrackEditorRequest::Reload);
    }
  }

  TEST_CASE("TrackPropertiesEditor - a value wider than its cell scrolls under the caret", "[tui][unit][editor]")
  {
    auto editor = makeEditor({TrackFixture{.title = "So What", .album = "Kind of Blue", .year = 1959}});

    focusRow(editor, "Title");
    typeText(editor, std::string(90, 'x') + "END");

    auto const rendered = renderElement(editor.render(), kTerminalColumns, kTerminalRows);
    auto const titleLine = lineIndexContaining(rendered.text, "* Title");
    REQUIRE(titleLine >= 0);

    // The value is far wider than an 80-column row, so a cell that only clipped
    // it would show neither what was just typed nor the caret sitting after it.
    CHECK(rendered.text.contains("END"));
    CHECK(hasCaretOnLine(rendered.screen, titleLine));
  }

  TEST_CASE("TrackPropertiesEditor - an empty input still shows where typing would land", "[tui][unit][editor]")
  {
    auto editor = makeEditor({
      TrackFixture{.title = "So What", .album = "Kind of Blue", .year = 1959},
      TrackFixture{.title = "Blue in Green", .album = "Kind of Blue", .year = 1959},
    });

    SECTION("A mixed field keeps its marker and takes a caret")
    {
      focusRow(editor, "Title");

      auto const rendered = renderElement(editor.render(), kTerminalColumns, kTerminalRows);
      auto const titleLine = lineIndexContaining(rendered.text, "Title");
      REQUIRE(titleLine >= 0);

      CHECK(rendered.text.contains("<Multiple Values>"));
      CHECK(hasCaretOnLine(rendered.screen, titleLine));
    }

    SECTION("An armed clear keeps its count and takes a caret")
    {
      focusRow(editor, "Title");
      editor.tryHandleEvent(clearEvent());

      auto const rendered = renderElement(editor.render(), kTerminalColumns, kTerminalRows);
      auto const titleLine = lineIndexContaining(rendered.text, "* Title");
      REQUIRE(titleLine >= 0);

      CHECK(rendered.text.contains("Clear for all 2 tracks"));
      CHECK(hasCaretOnLine(rendered.screen, titleLine));
    }
  }

  TEST_CASE("TrackPropertiesEditor - the Tracks page scrolls to its last target", "[tui][unit][editor]")
  {
    auto tracks = std::vector<TrackFixture>{};

    for (std::size_t index = 0; index < 30; ++index)
    {
      tracks.push_back(TrackFixture{.title = "Target " + std::to_string(index), .album = "Kind of Blue"});
    }

    auto editor = makeEditor(std::move(tracks));

    selectTab(editor, TrackEditorTab::Tracks);
    REQUIRE(editor.tab() == TrackEditorTab::Tracks);

    for (std::size_t step = 0; step < 40; ++step)
    {
      editor.tryHandleEvent(ftxui::Event::ArrowDown);
    }

    // Each target draws two lines, so a page that scrolled by target index
    // would run out of travel halfway down a capture this size.
    CHECK(frame(editor).contains("Target 29"));
  }

  TEST_CASE("TrackPropertiesEditor - centers the modal and fills a small terminal", "[tui][unit][editor]")
  {
    auto editor = makeEditor({TrackFixture{.title = "So What", .album = "Kind of Blue"}});

    SECTION("a wide terminal keeps the workspace visible around the box")
    {
      auto const rendered = renderElement(editor.renderModal(120, 40), 120, 40);
      auto const optTitleCells = findTextCells(rendered.screen, "Track Properties");

      REQUIRE(optTitleCells);
      // Inset on both axes: a full-surface panel would start at the first cell
      // of the first line inside its border.
      CHECK(optTitleCells->x_min > 1);
      CHECK(optTitleCells->x_max < 118);
      CHECK(lineIndexContaining(rendered.text, "Track Properties") > 1);
    }

    SECTION("80x24 gets the whole surface")
    {
      auto const rendered = renderElement(editor.renderModal(80, 24), 80, 24);
      auto const optTitleCells = findTextCells(rendered.screen, "Track Properties");

      REQUIRE(optTitleCells);
      CHECK(optTitleCells->x_min == 1);
      CHECK(lineIndexContaining(rendered.text, "Track Properties") == 1);
    }

    SECTION("the box does not resize as pages change")
    {
      auto const metadata = renderElement(editor.renderModal(120, 40), 120, 40);
      auto const optMetadataCells = findTextCells(metadata.screen, "Track Properties");
      auto const metadataLine = lineIndexContaining(metadata.text, "Track Properties");

      selectTab(editor, TrackEditorTab::Properties);

      auto const properties = renderElement(editor.renderModal(120, 40), 120, 40);
      auto const optPropertiesCells = findTextCells(properties.screen, "Track Properties");

      REQUIRE(optMetadataCells);
      REQUIRE(optPropertiesCells);
      CHECK(optPropertiesCells->x_min == optMetadataCells->x_min);
      CHECK(lineIndexContaining(properties.text, "Track Properties") == metadataLine);
    }
  }
} // namespace ao::tui::test
