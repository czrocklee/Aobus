// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/SmartListDialog.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include "test/unit/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/unit/linux-gtk/GtkWidgetTestSupport.h"
#include "track/TrackRowCache.h"
#include <ao/CoreIds.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/query/Expression.h>
#include <ao/query/Serializer.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/VirtualListIds.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/columnview.h>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/singleselection.h>
#include <gtkmm/window.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace ao::gtk::test
{
  TEST_CASE("SmartListDialog - renders the initial smart-list draft", "[gtk][unit][list][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto window = Gtk::Window{};
    auto& runtime = fixture.runtime();
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};

    auto dialog = SmartListDialog{window,
                                  runtime.library(),
                                  runtime.views(),
                                  runtime.sources(),
                                  runtime.completion(),
                                  ao::test::englishMessageCatalog(),
                                  rt::kAllTracksListId,
                                  cache};

    // Rebuild happens in idle task
    drainGtkEvents();

    CHECK(dialog.editListId() == kInvalidListId);
    CHECK(dialog.draft().expression.empty());

    bool foundTwoPane = false;
    bool foundConfigPane = false;
    bool foundPreviewPane = false;

    for (auto* const box : collectAll<Gtk::Box>(dialog))
    {
      foundTwoPane = foundTwoPane || box->has_css_class("ao-dialog-two-pane");
      foundConfigPane = foundConfigPane || box->has_css_class("ao-dialog-config-pane");
      foundPreviewPane = foundPreviewPane || box->has_css_class("ao-dialog-preview-pane");

      if (box->has_css_class("ao-dialog-config-pane"))
      {
        CHECK_FALSE(box->get_hexpand());
      }
    }

    CHECK(foundTwoPane);
    CHECK(foundConfigPane);
    CHECK(foundPreviewPane);
  }

  TEST_CASE("SmartListDialog - invalid preview source shows the acquisition failure", "[gtk][regression][list][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto window = Gtk::Window{};
    auto& runtime = fixture.runtime();
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto dialog = SmartListDialog{window,
                                  runtime.library(),
                                  runtime.views(),
                                  runtime.sources(),
                                  runtime.completion(),
                                  ao::test::englishMessageCatalog(),
                                  ListId{999999},
                                  cache};

    drainGtkEvents();

    bool visibleError = false;

    for (auto* const label : collectAll<Gtk::Label>(dialog))
    {
      visibleError =
        visibleError || (label->get_visible() && !label->get_text().empty() && label->has_css_class("ao-layout-error"));
    }

    CHECK(visibleError);
  }

  TEST_CASE("SmartListDialog - valid expression filters the transient preview projection",
            "[gtk][regression][smart-list][preview]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    addRuntimeTrack(fixture.runtime(), library::test::TrackSpec{.title = "Needle"});
    addRuntimeTrack(fixture.runtime(), library::test::TrackSpec{.title = "Haystack"});
    auto window = Gtk::Window{};
    auto& runtime = fixture.runtime();
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto dialog = SmartListDialog{window,
                                  runtime.library(),
                                  runtime.views(),
                                  runtime.sources(),
                                  runtime.completion(),
                                  ao::test::englishMessageCatalog(),
                                  rt::kAllTracksListId,
                                  cache};

    drainGtkEvents();
    dialog.setLocalExpression(R"($title = "Needle")");
    drainGtkEvents();

    auto const columnViews = collectAll<Gtk::ColumnView>(dialog);
    REQUIRE(columnViews.size() == 1);
    auto const selectionModelPtr = columnViews.front()->get_model();
    REQUIRE(selectionModelPtr);
    auto const singleSelectionPtr = std::dynamic_pointer_cast<Gtk::SingleSelection>(selectionModelPtr);
    REQUIRE(singleSelectionPtr);
    auto const previewModelPtr = singleSelectionPtr->get_model();
    REQUIRE(previewModelPtr);
    CHECK(previewModelPtr->get_n_items() == 1);
    CHECK(findLabelByText(dialog, "Showing all 1 match") != nullptr);
  }

  TEST_CASE("SmartListDialog - invalid expression rejects the transient preview",
            "[gtk][regression][smart-list][preview]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    addRuntimeTrack(fixture.runtime(), library::test::TrackSpec{.title = "Needle"});
    auto window = Gtk::Window{};
    auto& runtime = fixture.runtime();
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto dialog = SmartListDialog{window,
                                  runtime.library(),
                                  runtime.views(),
                                  runtime.sources(),
                                  runtime.completion(),
                                  ao::test::englishMessageCatalog(),
                                  rt::kAllTracksListId,
                                  cache};

    drainGtkEvents();
    dialog.setLocalExpression("(");
    drainGtkEvents();

    bool invalidEntry = false;
    bool visibleFilterError = false;

    for (auto* const entry : collectAll<Gtk::Entry>(dialog))
    {
      invalidEntry = invalidEntry || entry->has_css_class("ao-query-invalid");
    }

    for (auto* const label : collectAll<Gtk::Label>(dialog))
    {
      auto const text = label->get_text().raw();
      visibleFilterError = visibleFilterError || (label->get_visible() && label->has_css_class("ao-layout-error") &&
                                                  text.contains("Filter error:"));
    }

    CHECK(invalidEntry);
    CHECK(visibleFilterError);
  }

  TEST_CASE("SmartListDialog - valid local expression surfaces a stored parent filter error",
            "[gtk][regression][smart-list][preview]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto parentListId = kInvalidListId;
    auto fixture =
      GtkRuntimeFixture{[&parentListId](library::MusicLibrary& library)
                        {
                          auto transaction = library::test::writeTransaction(library);
                          auto builder = library::ListBuilder::makeEmpty().name("Broken parent").filter("(");
                          parentListId = ao::test::requireValue(transaction.apply(
                            [&builder](library::LibraryWrite& write) { return write.lists().create(builder); }));
                          REQUIRE(transaction.commit());
                        }};
    auto window = Gtk::Window{};
    auto& runtime = fixture.runtime();
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto dialog = SmartListDialog{window,
                                  runtime.library(),
                                  runtime.views(),
                                  runtime.sources(),
                                  runtime.completion(),
                                  ao::test::englishMessageCatalog(),
                                  parentListId,
                                  cache};

    drainGtkEvents();
    dialog.setLocalExpression("true");
    drainGtkEvents();

    bool visibleParentError = false;

    for (auto* const label : collectAll<Gtk::Label>(dialog))
    {
      auto const text = label->get_text().raw();
      visibleParentError =
        visibleParentError || (label->get_visible() && label->has_css_class("ao-layout-error") &&
                               text.contains("List " + std::to_string(parentListId.raw()) + " stored filter"));
    }

    CHECK(visibleParentError);
  }

  // The preview source is acquired from an idle task. Until it arrives the
  // dialog treats the expression as unvalidated, so the readiness pass must
  // re-run the full preview or naming a list never enables submission.
  TEST_CASE("SmartListDialog - naming a list enables submission once the preview source is ready",
            "[gtk][regression][list][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto window = Gtk::Window{};
    auto& runtime = fixture.runtime();
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};

    auto dialog = SmartListDialog{window,
                                  runtime.library(),
                                  runtime.views(),
                                  runtime.sources(),
                                  runtime.completion(),
                                  ao::test::englishMessageCatalog(),
                                  rt::kAllTracksListId,
                                  cache};

    drainGtkEvents();

    Gtk::Entry* nameEntry = nullptr;

    for (auto* const entry : collectAll<Gtk::Entry>(dialog))
    {
      if (entry->get_placeholder_text() == "List name")
      {
        nameEntry = entry;
        break;
      }
    }

    REQUIRE(nameEntry != nullptr);

    auto* const okButton = findButtonByLabel(dialog, "Create");
    REQUIRE(okButton != nullptr);
    CHECK_FALSE(okButton->get_sensitive());

    nameEntry->set_text("My List");
    drainGtkEvents();

    CHECK(okButton->get_sensitive());
  }

  TEST_CASE("SmartListDialog - pending submission remains guarded while the draft changes",
            "[gtk][regression][list][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto window = Gtk::Window{};
    auto& runtime = fixture.runtime();
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto dialog = SmartListDialog{window,
                                  runtime.library(),
                                  runtime.views(),
                                  runtime.sources(),
                                  runtime.completion(),
                                  ao::test::englishMessageCatalog(),
                                  rt::kAllTracksListId,
                                  cache};

    drainGtkEvents();

    Gtk::Entry* nameEntry = nullptr;

    for (auto* const entry : collectAll<Gtk::Entry>(dialog))
    {
      if (entry->get_placeholder_text() == "List name")
      {
        nameEntry = entry;
        break;
      }
    }

    REQUIRE(nameEntry != nullptr);
    auto* const okButton = findButtonByLabel(dialog, "Create");
    REQUIRE(okButton != nullptr);

    nameEntry->set_text("First draft");
    drainGtkEvents();
    REQUIRE(okButton->get_sensitive());

    CHECK(dialog.beginSubmission());
    CHECK_FALSE(okButton->get_sensitive());
    CHECK_FALSE(dialog.beginSubmission());

    nameEntry->set_text("Changed while pending");
    drainGtkEvents();
    CHECK_FALSE(okButton->get_sensitive());

    dialog.completeSubmission();
    CHECK(okButton->get_sensitive());
  }

  // A list created from the library root, and any top-level list opened for
  // editing, carries parentId kInvalidListId. The source cache rejects that id
  // outright, so the dialog must resolve it to the All Tracks root or the
  // preview never builds and the editor opens showing an acquisition error.
  TEST_CASE("SmartListDialog - root parent previews against All Tracks", "[gtk][regression][list][dialog]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto window = Gtk::Window{};
    auto& runtime = fixture.runtime();
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};

    auto dialog = SmartListDialog{window,
                                  runtime.library(),
                                  runtime.views(),
                                  runtime.sources(),
                                  runtime.completion(),
                                  ao::test::englishMessageCatalog(),
                                  kInvalidListId,
                                  cache};

    drainGtkEvents();

    bool visibleError = false;

    for (auto* const label : collectAll<Gtk::Label>(dialog))
    {
      visibleError =
        visibleError || (label->get_visible() && !label->get_text().empty() && label->has_css_class("ao-layout-error"));
    }

    CHECK_FALSE(visibleError);

    Gtk::Entry* nameEntry = nullptr;

    for (auto* const entry : collectAll<Gtk::Entry>(dialog))
    {
      if (entry->get_placeholder_text() == "List name")
      {
        nameEntry = entry;
        break;
      }
    }

    REQUIRE(nameEntry != nullptr);

    auto* const okButton = findButtonByLabel(dialog, "Create");
    REQUIRE(okButton != nullptr);

    nameEntry->set_text("Root List");
    drainGtkEvents();

    CHECK(okButton->get_sensitive());
  }

  TEST_CASE("SmartListDialog - Playlist template exposes a visible tag and chooses Manual Order",
            "[gtk][unit][smart-list-dialog][playlist]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto window = Gtk::Window{};
    auto& runtime = fixture.runtime();
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto dialog = SmartListDialog{window,
                                  runtime.library(),
                                  runtime.views(),
                                  runtime.sources(),
                                  runtime.completion(),
                                  ao::test::englishMessageCatalog(),
                                  rt::kAllTracksListId,
                                  cache};

    dialog.configurePlaylistTemplate("Road Trip");
    drainGtkEvents();

    auto* const membershipTagLabel = findLabelByText(dialog, "Membership Tag");
    REQUIRE(membershipTagLabel != nullptr);
    CHECK(membershipTagLabel->get_visible());
    CHECK(dialog.get_title() == "New Playlist");
    CHECK(dialog.presentationId() == "list-order");
    CHECK(dialog.draft().expression ==
          query::serialize(query::VariableExpression{.type = query::VariableType::Tag, .name = "Road Trip"}));
  }

  TEST_CASE("SmartListDialog - retained presentation callback retires with the dialog",
            "[gtk][regression][smart-list-dialog][concurrency]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto window = Gtk::Window{};
    auto& runtime = fixture.runtime();
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto callback = std::function<void()>{};
    std::size_t presentationCount = 0;

    {
      auto dialog = SmartListDialog{window,
                                    runtime.library(),
                                    runtime.views(),
                                    runtime.sources(),
                                    runtime.completion(),
                                    ao::test::englishMessageCatalog(),
                                    rt::kAllTracksListId,
                                    cache};
      drainGtkEvents();
      callback = dialog.guardPresentationCallback([&presentationCount] { ++presentationCount; });
      callback();
      CHECK(presentationCount == 1);
    }

    callback();
    CHECK(presentationCount == 1);
  }
} // namespace ao::gtk::test
