// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "RenderTestSupport.h"
#include "TrackPropertiesEditorTestSupport.h"
#include "test/unit/MessageCatalogTestSupport.h"
#include "tui/TrackPropertiesEditor.h"
#include "tui/TrackTagEditor.h"

#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>

#include <cstddef>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace ao::tui::test
{
  TEST_CASE("TrackTagEditor - pending changes retain the full large-selection fraction", "[tui][regression][editor]")
  {
    auto editor = TrackTagEditor{ao::test::englishMessageCatalog(), 2000, {{"Jazz", 1500}}, {}};
    editor.handleEvent(ftxui::Event::Return);
    auto rendered = renderElement(editor.render(), 80, 24);
    CHECK(rendered.text.contains("1500/2000 Add"));
    editor.handleEvent(ftxui::Event::Return);
    rendered = renderElement(editor.render(), 80, 24);
    CHECK(rendered.text.contains("1500/2000 Remove"));
  }

  TEST_CASE("TrackPropertiesEditor - edits tags through one always-live query", "[tui][unit][editor]")
  {
    auto editor = makeEditor(
      {
        TrackFixture{.title = "So What", .album = "Kind of Blue"},
        TrackFixture{.title = "Blue in Green", .album = "Kind of Blue"},
      },
      {},
      {{"Jazz", 2}, {"Modal", 1}},
      {"Acoustic"});

    selectTab(editor, TrackEditorTab::Tags);
    REQUIRE(editor.tab() == TrackEditorTab::Tags);

    auto const initialFrame = frame(editor);
    // The box carries the state; the fraction appears only where the box
    // cannot say it, which is a tag some but not all targets carry.
    CHECK(initialFrame.contains("[x] Jazz"));
    CHECK(initialFrame.contains("[~] Modal"));
    CHECK(initialFrame.contains("1/2"));
    // Library tags the selection does not carry need no heading of their own:
    // an empty box already says no captured target carries the tag.
    CHECK(initialFrame.contains("[ ] Acoustic"));

    SECTION("Enter adds a tag only some targets carry")
    {
      editor.tryHandleEvent(ftxui::Event::ArrowDown); // focus "Modal" (row 1)
      editor.tryHandleEvent(ftxui::Event::Return);

      CHECK(editor.isDirty());
      CHECK(editor.canApply());
      auto const text = frame(editor);
      CHECK(text.contains("[x] Modal"));
      CHECK(text.contains("Add"));
      CHECK(editor.patchSummary().tagAddCount == 1);

      auto const patch = editor.buildPatch();
      REQUIRE(patch.tagsToAdd.size() == 1);
      CHECK(patch.tagsToAdd[0] == "Modal");
      CHECK(patch.tagsToRemove.empty());
    }

    SECTION("Enter cycles a mixed tag through both directions and back")
    {
      editor.tryHandleEvent(ftxui::Event::ArrowDown); // focus "Modal" (1 of 2)
      editor.tryHandleEvent(ftxui::Event::Return);
      CHECK(frame(editor).contains("[x] Modal"));

      // Only a mixed tag has a real choice between the two directions, so only
      // a mixed tag gets a third stop on the cycle.
      editor.tryHandleEvent(ftxui::Event::Return);
      CHECK(frame(editor).contains("[ ] Modal"));
      // The box now shows the destination, so the fraction is the only place
      // the tag's starting point survives.
      CHECK(frame(editor).contains("1/2"));
      CHECK(editor.patchSummary().tagRemoveCount == 1);
      CHECK(editor.patchSummary().tagAddCount == 0);

      editor.tryHandleEvent(ftxui::Event::Return);
      CHECK_FALSE(editor.isDirty());
    }

    SECTION("Enter removes a tag every target carries, skipping a no-op add")
    {
      editor.tryHandleEvent(ftxui::Event::Return); // focus is on "Jazz" (2 of 2)

      CHECK(editor.isDirty());
      auto const text = frame(editor);
      CHECK(text.contains("[ ] Jazz"));
      CHECK(text.contains("Remove"));
      CHECK(editor.patchSummary().tagRemoveCount == 1);

      auto const patch = editor.buildPatch();
      REQUIRE(patch.tagsToRemove.size() == 1);
      CHECK(patch.tagsToRemove[0] == "Jazz");
      CHECK(patch.tagsToAdd.empty());

      editor.tryHandleEvent(restoreEvent());
      CHECK_FALSE(editor.isDirty());
      CHECK(editor.patchSummary().tagRemoveCount == 0);
    }

    SECTION("Enter adds a suggested tag no target carries, skipping a no-op remove")
    {
      editor.tryHandleEvent(ftxui::Event::ArrowDown);
      editor.tryHandleEvent(ftxui::Event::ArrowDown); // focus "Acoustic" (0 of 2)
      editor.tryHandleEvent(ftxui::Event::Return);

      CHECK(frame(editor).contains("[x] Acoustic"));

      editor.tryHandleEvent(ftxui::Event::Return);
      CHECK_FALSE(editor.isDirty());
    }

    SECTION("Typing filters without entering a mode")
    {
      typeText(editor, "mod");

      auto const text = frame(editor);
      CHECK(text.contains("Modal"));
      CHECK_FALSE(text.contains("Jazz"));
      CHECK_FALSE(text.contains("Acoustic"));

      // The first match is selected, so Enter needs no navigation to reach it.
      editor.tryHandleEvent(ftxui::Event::Return);
      auto const patch = editor.buildPatch();
      REQUIRE(patch.tagsToAdd.size() == 1);
      CHECK(patch.tagsToAdd[0] == "Modal");
    }

    SECTION("A committed edit drops the query and keeps the tag selected")
    {
      typeText(editor, "mod");
      editor.tryHandleEvent(ftxui::Event::Return);

      // The list returns to its full form so the change is read in context.
      auto const text = frame(editor);
      CHECK(text.contains("Jazz"));
      CHECK(text.contains("Acoustic"));
      CHECK(text.contains("[x] Modal"));

      // Selection followed the tag, so a second Enter continues its cycle
      // rather than acting on whichever row happens to be first.
      editor.tryHandleEvent(ftxui::Event::Return);
      CHECK(frame(editor).contains("[ ] Modal"));
    }

    SECTION("A query naming no tag offers to create it")
    {
      typeText(editor, "Bebop");

      auto const text = frame(editor);
      CHECK(text.contains("Add new tag \"Bebop\""));

      editor.tryHandleEvent(ftxui::Event::Return);

      CHECK(editor.isDirty());
      auto const patch = editor.buildPatch();
      REQUIRE(patch.tagsToAdd.size() == 1);
      CHECK(patch.tagsToAdd[0] == "Bebop");
      // A created tag joins the selection's own tags rather than being buried
      // under the library suggestions.
      auto const committed = frame(editor);
      CHECK(lineIndexContaining(committed, "Bebop") < lineIndexContaining(committed, "Acoustic"));
    }

    SECTION("Creation trails the matches so Enter prefers an existing tag")
    {
      // "Mod" is a prefix of "Modal" but names no tag itself, so both the match
      // and the offer to create are on screen at once.
      typeText(editor, "Mod");
      auto const text = frame(editor);
      REQUIRE(text.contains("Modal"));
      REQUIRE(text.contains("Add new tag \"Mod\""));

      editor.tryHandleEvent(ftxui::Event::Return);
      auto const patch = editor.buildPatch();
      REQUIRE(patch.tagsToAdd.size() == 1);
      CHECK(patch.tagsToAdd[0] == "Modal");
    }

    SECTION("Escape drops the query before it means anything about the editor")
    {
      typeText(editor, "Unfinished");

      editor.tryHandleEvent(ftxui::Event::Escape);
      CHECK(editor.takeRequest() == TrackEditorRequest::None);
      CHECK_FALSE(editor.isDirty());
      CHECK(editor.buildPatch().tagsToAdd.empty());
      CHECK(frame(editor).contains("Jazz"));

      // With the query gone, the next Escape is about the editor again.
      editor.tryHandleEvent(ftxui::Event::Escape);
      CHECK(editor.takeRequest() == TrackEditorRequest::Close);
    }

    SECTION("Switching away drops the query")
    {
      typeText(editor, "mod");
      REQUIRE_FALSE(frame(editor).contains("Jazz"));

      selectTab(editor, TrackEditorTab::Metadata);
      selectTab(editor, TrackEditorTab::Tags);

      CHECK(frame(editor).contains("Jazz"));
      CHECK(frame(editor).contains("Acoustic"));
    }
  }

  TEST_CASE("TrackPropertiesEditor - matches a decomposed tag query against the stored name", "[tui][unit][editor]")
  {
    auto editor = makeEditor({TrackFixture{.title = "So What", .album = "Kind of Blue"}}, {}, {{"Caf\u00e9", 1}});

    selectTab(editor, TrackEditorTab::Tags);
    REQUIRE(frame(editor).contains("[x] Caf\u00e9"));

    // The same name typed decomposed, which is what a terminal delivers from
    // some input methods. Tag names reach storage in NFC, so matching the typed
    // bytes would read the tag as one nobody carries.
    typeText(editor, "Cafe");
    editor.tryHandleEvent(ftxui::Event::Character(std::string{"\u0301"}));

    auto const text = frame(editor);
    CHECK(text.contains("Caf\u00e9"));
    // Creating here would give one stored identity two rows with independent
    // intents, so an add and a remove could be submitted for the same tag.
    CHECK_FALSE(text.contains("Add new tag"));

    editor.tryHandleEvent(ftxui::Event::Return);

    auto const patch = editor.buildPatch();
    CHECK(patch.tagsToAdd.empty());
    REQUIRE(patch.tagsToRemove.size() == 1);
    CHECK(patch.tagsToRemove[0] == "Caf\u00e9");
  }

  TEST_CASE("TrackPropertiesEditor - keeps the tag query one line tall", "[tui][unit][editor]")
  {
    auto editor = makeEditor(
      {
        TrackFixture{.title = "So What", .album = "Kind of Blue"},
        TrackFixture{.title = "Blue in Green", .album = "Kind of Blue"},
      },
      {},
      {{"Jazz", 2}, {"Modal", 1}},
      {"Acoustic"});

    selectTab(editor, TrackEditorTab::Tags);

    SECTION("The list, not the field, takes the space the page has spare")
    {
      typeText(editor, "baest");

      auto const rendered = renderElement(editor.render(), kTerminalColumns, kTerminalRows);
      auto const optCaretCells = findTextCells(rendered.screen, "baest");
      REQUIRE(optCaretCells);

      // One line of query, one rule, then the list. A field that flexed on both
      // axes would push the list down the page and drag the caret into a
      // column.
      auto const queryLine = lineIndexContaining(rendered.text, "baest");
      CHECK(lineIndexContaining(rendered.text, "Add new tag") == queryLine + 2);
    }

    SECTION("A query wider than the modal scrolls to the caret")
    {
      typeText(editor, std::string(90, 'x') + "END");

      auto const rendered = renderElement(editor.render(), kTerminalColumns, kTerminalRows);
      auto const queryLine = lineIndexContaining(rendered.text, "END");
      REQUIRE(queryLine >= 0);

      // A field that shrank its segments instead would lose the tail and the
      // caret with it, so further typing would land out of sight.
      CHECK(hasCaretOnLine(rendered.screen, queryLine));
      // Scrolling is horizontal only: still one line of query, one rule, then
      // the list.
      CHECK(lineIndexContaining(rendered.text, "Add new tag") == queryLine + 2);
    }

    SECTION("An empty query names itself where the label used to be")
    {
      auto const text = frame(editor);

      CHECK(text.contains("type to filter or add a tag"));
      CHECK_FALSE(text.contains("Search / add"));
    }

    SECTION("The footer names what Enter would do on the selected row")
    {
      CHECK(frame(editor).contains("toggle"));

      typeText(editor, "Bebop");
      auto const creating = frame(editor);
      CHECK(creating.contains("add"));
      CHECK_FALSE(creating.contains("toggle"));
    }
  }

  TEST_CASE("TrackPropertiesEditor - caps the suggestions drawn, never the vocabulary searched", "[tui][unit][editor]")
  {
    // Ranked below the cap by construction: the controller hands suggestions
    // over in descending frequency, so position in this vector is the ranking.
    auto suggestions = std::vector<std::string>{};

    for (std::size_t i = 0; i < 60; ++i)
    {
      suggestions.push_back(std::format("Sugg{:02}", i));
    }

    auto editor =
      makeEditor({TrackFixture{.title = "So What", .album = "Kind of Blue"}}, {}, {{"Jazz", 1}}, suggestions);
    selectTab(editor, TrackEditorTab::Tags);

    SECTION("The list stops at the cap and says how much it left out")
    {
      CHECK(frame(editor).contains("10 more tags not shown"));

      // Walking to the end of the list reaches the cap and stops there, which
      // the viewport alone could not show.
      for (std::size_t i = 0; i < 10; ++i)
      {
        editor.tryHandleEvent(ftxui::Event::PageDown);
      }

      auto const text = frame(editor);
      CHECK(text.contains("Sugg49"));
      CHECK_FALSE(text.contains("Sugg50"));
    }

    SECTION("A tag past the cap is still reachable by name")
    {
      typeText(editor, "Sugg57");

      auto const text = frame(editor);
      CHECK(text.contains("Sugg57"));
      // Found, not invented: a tag the library already has is never offered
      // for creation.
      CHECK_FALSE(text.contains("Add new tag"));

      editor.tryHandleEvent(ftxui::Event::Return);
      auto const patch = editor.buildPatch();
      REQUIRE(patch.tagsToAdd.size() == 1);
      CHECK(patch.tagsToAdd[0] == "Sugg57");
    }

    SECTION("A marked tag survives the cap once the query is gone")
    {
      typeText(editor, "Sugg57");
      editor.tryHandleEvent(ftxui::Event::Return);

      // Dropping the query returns the full list, where this tag ranks past
      // the cap; the draft it now carries is what keeps it on screen.
      auto const text = frame(editor);
      CHECK(text.contains("[x] Sugg57"));
      CHECK(text.contains("Add"));

      // Restoring takes that exemption away, so the row goes back under the cap
      // rather than lingering until the next keystroke edits the query.
      editor.tryHandleEvent(restoreEvent());
      CHECK_FALSE(editor.isDirty());
      CHECK_FALSE(frame(editor).contains("Sugg57"));
    }
  }

  TEST_CASE("TrackPropertiesEditor - finds exact Unicode tags without creating case variants",
            "[tui][regression][editor]")
  {
    for (auto const& [name, query] :
         std::vector<std::pair<std::string, std::string>>{{"Über", "über"}, {"Été", "été"}, {"Straße", "STRASSE"}})
    {
      DYNAMIC_SECTION(name << " matched by " << query)
      {
        auto editor = makeEditor({TrackFixture{.title = "Track", .album = "Blue"}}, {}, {}, {name});
        selectTab(editor, TrackEditorTab::Tags);
        typeText(editor, query);
        CHECK(frame(editor).contains(name));
        CHECK_FALSE(editor.isDirty());

        editor.tryHandleEvent(ftxui::Event::Return);
        CHECK(editor.buildPatch().tagsToAdd == std::vector<std::string>{name});
      }
    }
  }

  TEST_CASE("TrackPropertiesEditor - keeps an exact tag reachable beyond matching suggestions",
            "[tui][regression][editor]")
  {
    auto suggestions = std::vector<std::string>{};

    for (std::size_t index = 0; index < 50; ++index)
    {
      suggestions.push_back(std::format("Jazz variant {:02}", index));
    }

    suggestions.emplace_back("Jazz");
    auto editor = makeEditor({TrackFixture{.title = "Track", .album = "Blue"}}, {}, {}, std::move(suggestions));
    selectTab(editor, TrackEditorTab::Tags);
    typeText(editor, "JAZZ");

    for (std::size_t page = 0; page < 7; ++page)
    {
      editor.tryHandleEvent(ftxui::Event::PageDown);
    }

    CHECK(frame(editor).contains("> [ ] Jazz "));
    editor.tryHandleEvent(ftxui::Event::Return);
    CHECK(editor.buildPatch().tagsToAdd == std::vector<std::string>{"Jazz"});
  }

  TEST_CASE("TrackPropertiesEditor - a blank tag query never creates an invisible tag", "[tui][regression][editor]")
  {
    auto editor = makeEditor({TrackFixture{.title = "Track", .album = "Blue"}});
    selectTab(editor, TrackEditorTab::Tags);
    typeText(editor, "   ");
    editor.tryHandleEvent(ftxui::Event::Return);

    CHECK_FALSE(editor.isDirty());
    CHECK(editor.buildPatch().tagsToAdd.empty());
  }

  TEST_CASE("TrackPropertiesEditor - separates full-width tag names from membership counts",
            "[tui][regression][editor]")
  {
    auto const name = std::string(32, 'x');
    auto editor =
      makeEditor({TrackFixture{.title = "First", .album = "Blue"}, TrackFixture{.title = "Second", .album = "Blue"}},
                 {},
                 {{name, 1}});
    selectTab(editor, TrackEditorTab::Tags);

    CHECK(frame(editor).contains(name + " 1/2"));
  }
} // namespace ao::tui::test
