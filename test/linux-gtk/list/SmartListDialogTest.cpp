// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "list/SmartListDialog.h"

#include "test/MessageCatalogTestSupport.h"
#include "test/TestFixtureSupport.h"
#include "test/library/TrackTestSupport.h"
#include "test/library/WritableLibraryTestSupport.h"
#include "test/linux-gtk/GtkApplicationTestSupport.h"
#include "test/linux-gtk/GtkRuntimeTestSupport.h"
#include "test/linux-gtk/GtkWidgetTestSupport.h"
#include "track/TrackRowCache.h"
#include <ao/CoreIds.h>
#include <ao/library/LibraryWrite.h>
#include <ao/library/ListBuilder.h>
#include <ao/query/Expression.h>
#include <ao/query/Serializer.h>
#include <ao/rt/AppRuntime.h>
#include <ao/rt/ListMutation.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/VirtualListIds.h>
#include <ao/rt/WorkspaceService.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryCommands.h>
#include <ao/rt/library/LibrarySnapshot.h>

#include <catch2/catch_test_macros.hpp>
#include <giomm/listmodel.h>
#include <glibmm/refptr.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/columnview.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/listview.h>
#include <gtkmm/singleselection.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/window.h>
#include <pangomm/layout.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ao::gtk::test
{
  namespace
  {
    Glib::RefPtr<Gio::ListModel> previewRows(SmartListDialog& dialog)
    {
      auto* const preview = findWidget<Gtk::ColumnView>(dialog);
      REQUIRE(preview != nullptr);
      auto const modelPtr = std::dynamic_pointer_cast<Gio::ListModel>(preview->get_model());
      REQUIRE(modelPtr);
      return modelPtr;
    }

    Gtk::Entry& requireEntryByPlaceholder(SmartListDialog& dialog, std::string_view const placeholder)
    {
      auto const entries = collectAll<Gtk::Entry>(dialog);
      auto const it = std::ranges::find_if(
        entries, [placeholder](Gtk::Entry const* entry) { return entry->get_placeholder_text().raw() == placeholder; });
      REQUIRE(it != entries.end());
      return **it;
    }
  } // namespace

  TEST_CASE("SmartListDialog - renders the initial smart-list draft")
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
                                  runtime.workspace().customPresets(),
                                  ao::test::englishMessageCatalog(),
                                  rt::kAllTracksListId,
                                  cache};

    // Rebuild happens in idle task
    drainGtkEvents();

    CHECK(dialog.editListId() == kInvalidListId);
    CHECK(dialog.draft().expression.empty());
    CHECK(previewRows(dialog)->get_n_items() == 0);
    CHECK(findLabelByText(dialog, "No tracks in library") != nullptr);

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

  TEST_CASE("SmartListDialog - invalid preview source shows the acquisition failure")
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
                                  runtime.workspace().customPresets(),
                                  ao::test::englishMessageCatalog(),
                                  ListId{999999},
                                  cache};

    drainGtkEvents();

    auto* const errorLabel = findWidgetByClass<Gtk::Label>(dialog, "ao-layout-error");
    REQUIRE(errorLabel != nullptr);
    CHECK(errorLabel->get_visible());
    CHECK(errorLabel->get_text() == "Preview unavailable: List 999999 does not exist");
    CHECK(previewRows(dialog)->get_n_items() == 0);
  }

  TEST_CASE("SmartListDialog - valid expression filters the transient preview projection")
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
                                  runtime.workspace().customPresets(),
                                  ao::test::englishMessageCatalog(),
                                  rt::kAllTracksListId,
                                  cache};

    drainGtkEvents();
    dialog.setLocalExpression(R"($title = "Needle")");
    drainGtkEvents();

    CHECK(previewRows(dialog)->get_n_items() == 1);
    CHECK(findLabelByText(dialog, "Showing all matches: 1") != nullptr);
  }

  TEST_CASE("SmartListDialog - a title edit refreshes the mapped preview label")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto const trackId = addRuntimeTrack(runtime, library::test::TrackSpec{.title = "Original"});
    auto window = Gtk::Window{};
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto dialog = SmartListDialog{window,
                                  runtime.library(),
                                  runtime.views(),
                                  runtime.sources(),
                                  runtime.completion(),
                                  runtime.workspace().customPresets(),
                                  ao::test::englishMessageCatalog(),
                                  rt::kAllTracksListId,
                                  cache};
    dialog.setLocalExpression(R"($artist = "Artist")");
    dialog.present();

    auto* const preview = findWidget<Gtk::ColumnView>(dialog);
    REQUIRE(preview != nullptr);
    REQUIRE(tryPumpGtkEventsUntil(
      [&]
      {
        auto* const label = findLabelByText(*preview, "Original - Artist (Album)");
        return label != nullptr && label->get_mapped();
      }));
    REQUIRE(previewRows(dialog)->get_n_items() == 1);

    updateRuntimeTrack(runtime, trackId, [](library::test::TrackSpec& spec) { spec.title = "Edited"; });

    CHECK(previewRows(dialog)->get_n_items() == 1);
    auto* const editedLabel = findLabelByText(*preview, "Edited - Artist (Album)");
    REQUIRE(editedLabel != nullptr);
    CHECK(editedLabel->get_mapped());
  }

  TEST_CASE("SmartListDialog - populating an Auto edit immediately refreshes its filter preview")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    addRuntimeTrack(runtime, library::test::TrackSpec{.title = "Needle"});
    auto const listId = ao::test::requireValue(runGtkTask(
      runtime,
      runtime.library().commands().createListAsync(rt::ListDraft{.name = "Empty List", .expression = "false"})));
    auto const optNode = runtime.library().snapshot().listNode(listId);
    REQUIRE(optNode);

    auto window = Gtk::Window{};
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto dialog = SmartListDialog{window,
                                  runtime.library(),
                                  runtime.views(),
                                  runtime.sources(),
                                  runtime.completion(),
                                  runtime.workspace().customPresets(),
                                  ao::test::englishMessageCatalog(),
                                  kInvalidListId,
                                  cache};
    drainGtkEvents();
    auto const previewModelPtr = previewRows(dialog);
    REQUIRE(previewModelPtr->get_n_items() == 1);

    // Auto remains selected, so only population itself can refresh before the timeout.
    dialog.populate(listId, *optNode, std::nullopt);
    CHECK(previewModelPtr->get_n_items() == 0);
    CHECK(findLabelByText(dialog, "No matches") != nullptr);
  }

  TEST_CASE("SmartListDialog - limits the preview rows without losing the full match count")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();

    for (std::size_t index = 0; index < 10; ++index)
    {
      addRuntimeTrack(runtime, library::test::TrackSpec{.title = "Track " + std::to_string(index)});
    }

    auto window = Gtk::Window{};
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto dialog = SmartListDialog{window,
                                  runtime.library(),
                                  runtime.views(),
                                  runtime.sources(),
                                  runtime.completion(),
                                  runtime.workspace().customPresets(),
                                  ao::test::englishMessageCatalog(),
                                  rt::kAllTracksListId,
                                  cache};

    dialog.setLocalExpression("true");
    drainGtkEvents();

    auto const previewModelPtr = previewRows(dialog);
    CHECK(previewModelPtr->get_n_items() == 10);
    auto* const statusLabel = findLabelByText(dialog, "Showing all matches: 10");
    REQUIRE(statusLabel != nullptr);
    CHECK(statusLabel->get_visible());
    auto* const errorLabel = findWidgetByClass<Gtk::Label>(dialog, "ao-layout-error");
    REQUIRE(errorLabel != nullptr);
    CHECK_FALSE(errorLabel->get_visible());

    dialog.showError("Save failed");
    // Rebuilding the same draft does not count as an edit.
    dialog.setLocalExpression("true");
    CHECK(errorLabel->get_visible());
    CHECK(errorLabel->get_text() == "Save failed");

    // A live append past the slice changes the total, not the sample size or save feedback.
    addRuntimeTrack(runtime, library::test::TrackSpec{.title = "Track 10"});
    drainGtkEvents();
    CHECK(previewModelPtr->get_n_items() == 10);
    CHECK(statusLabel->get_text() == "Preview: 10 of 11 matches");
    CHECK(errorLabel->get_visible());
    CHECK(errorLabel->get_text() == "Save failed");

    dialog.showError({});
    addRuntimeTrack(runtime, library::test::TrackSpec{.title = "Track 11"});
    drainGtkEvents();
    CHECK(previewModelPtr->get_n_items() == 10);
    CHECK(statusLabel->get_text() == "Preview: 10 of 12 matches");
    CHECK_FALSE(errorLabel->get_visible());

    dialog.setLocalExpression("");
    CHECK(previewModelPtr->get_n_items() == 10);
    CHECK(statusLabel->get_text() == "Preview: 10 of 12 library tracks");
    CHECK_FALSE(errorLabel->get_visible());

    dialog.setLocalExpression("false");
    CHECK(previewModelPtr->get_n_items() == 0);
    CHECK(statusLabel->get_text() == "No matches");
    CHECK_FALSE(errorLabel->get_visible());
  }

  TEST_CASE("SmartListDialog - deleting the parent shows the preview blocker alongside the Save error")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    addRuntimeTrack(runtime, library::test::TrackSpec{.title = "Needle"});
    addRuntimeTrack(runtime, library::test::TrackSpec{.title = "Haystack"});
    auto& commands = runtime.library().commands();
    auto const parentListId = ao::test::requireValue(runGtkTask(
      runtime, commands.createListAsync(rt::ListDraft{.name = "Parent", .expression = R"($title = "Needle")"})));

    auto window = Gtk::Window{};
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};
    auto dialog = SmartListDialog{window,
                                  runtime.library(),
                                  runtime.views(),
                                  runtime.sources(),
                                  runtime.completion(),
                                  runtime.workspace().customPresets(),
                                  ao::test::englishMessageCatalog(),
                                  parentListId,
                                  cache};
    drainGtkEvents();

    auto const previewModelPtr = previewRows(dialog);
    CHECK(previewModelPtr->get_n_items() == 1);
    auto* const statusLabel = findLabelByText(dialog, "Showing all tracks from source: 1");
    auto* const errorLabel = findWidgetByClass<Gtk::Label>(dialog, "ao-layout-error");
    auto* const createButton = findButtonByLabel(dialog, "Create");
    REQUIRE(statusLabel != nullptr);
    REQUIRE(errorLabel != nullptr);
    REQUIRE(createButton != nullptr);
    requireEntryByPlaceholder(dialog, "List name").set_text("Child draft");
    REQUIRE(createButton->get_sensitive());
    dialog.showError("Create list is busy");

    REQUIRE(runGtkTask(runtime, commands.deleteListAsync(parentListId)));
    drainGtkEvents();
    CHECK(previewModelPtr->get_n_items() == 0);
    CHECK(statusLabel->get_text().empty());
    CHECK_FALSE(createButton->get_sensitive());
    CHECK(errorLabel->get_visible());
    auto const unavailable = "Preview unavailable: List " + std::to_string(parentListId.raw()) + " does not exist";
    auto const expectedError = "Create list is busy\n" + unavailable;
    REQUIRE(errorLabel->get_text() == expectedError);

    auto const dropDowns = collectAll<Gtk::DropDown>(dialog);
    REQUIRE(dropDowns.size() == 1);
    dropDowns.front()->set_selected(1U);
    REQUIRE_FALSE(dialog.presentationId().empty());
    CHECK_FALSE(createButton->get_sensitive());
    CHECK(errorLabel->get_visible());
    CHECK(errorLabel->get_text() == expectedError);

    // A response still revalidates and drops the old writer error, but isn't needed to see the blocker.
    CHECK_FALSE(dialog.tryBeginSubmission());
    CHECK_FALSE(createButton->get_sensitive());
    CHECK(errorLabel->get_visible());
    CHECK(errorLabel->get_text() == unavailable);
    CHECK(dialog.draft().name == "Child draft");
  }

  TEST_CASE("SmartListDialog - invalid expression rejects the transient preview")
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
                                  runtime.workspace().customPresets(),
                                  ao::test::englishMessageCatalog(),
                                  rt::kAllTracksListId,
                                  cache};

    drainGtkEvents();
    auto const previewModelPtr = previewRows(dialog);
    REQUIRE(previewModelPtr->get_n_items() == 1);
    dialog.setLocalExpression("(");
    drainGtkEvents();
    CHECK(previewModelPtr->get_n_items() == 0);

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

  TEST_CASE("SmartListDialog - valid local expression surfaces a stored parent filter error")
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
                                  runtime.workspace().customPresets(),
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
  TEST_CASE("SmartListDialog - naming a list enables submission once the preview source is ready")
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
                                  runtime.workspace().customPresets(),
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

  TEST_CASE("SmartListDialog - pending submission remains guarded while the draft changes")
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
                                  runtime.workspace().customPresets(),
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

    CHECK(dialog.tryBeginSubmission());
    CHECK_FALSE(okButton->get_sensitive());
    CHECK_FALSE(dialog.tryBeginSubmission());

    nameEntry->set_text("Changed while pending");
    drainGtkEvents();
    CHECK_FALSE(okButton->get_sensitive());

    dialog.completeSubmission();
    CHECK(okButton->get_sensitive());
  }

  TEST_CASE("SmartListDialog - editing draft fields dismisses the previous submission error")
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
                                  runtime.workspace().customPresets(),
                                  ao::test::englishMessageCatalog(),
                                  rt::kAllTracksListId,
                                  cache};
    drainGtkEvents();

    auto* const errorLabel = findWidgetByClass<Gtk::Label>(dialog, "ao-layout-error");
    REQUIRE(errorLabel != nullptr);
    auto const showFailure = [&]
    {
      dialog.showError("Create list is busy");
      REQUIRE(errorLabel->get_visible());
    };

    SECTION("name in a Smart List")
    {
      auto& name = requireEntryByPlaceholder(dialog, "List name");
      name.set_text("First name");
      showFailure();
      name.set_text("New name");
      CHECK(dialog.draft().name == "New name");
    }

    SECTION("name in a Playlist with a separately chosen tag")
    {
      dialog.configurePlaylistTemplate("First name", "Fixed tag");
      auto const expression = dialog.draft().expression;
      showFailure();
      requireEntryByPlaceholder(dialog, "List name").set_text("New name");
      CHECK(dialog.draft().name == "New name");
      CHECK(dialog.draft().expression == expression);
    }

    SECTION("description")
    {
      requireEntryByPlaceholder(dialog, "List name").set_text("List draft");
      showFailure();
      requireEntryByPlaceholder(dialog, "Optional description").set_text("New description");
      CHECK(dialog.draft().description == "New description");
    }

    SECTION("membership tag")
    {
      dialog.configurePlaylistTemplate("Playlist", "FirstTag");
      showFailure();
      requireEntryByPlaceholder(dialog, "Visible tag used for membership").set_text("Another tag");
      CHECK(dialog.draft().expression ==
            query::serialize(query::VariableExpression{.type = query::VariableType::Tag, .name = "Another tag"}));
    }

    SECTION("local filter before debounce")
    {
      requireEntryByPlaceholder(dialog, "List name").set_text("List draft");
      auto* const filter = findWidgetByClass<Gtk::Entry>(dialog, "ao-query-expression-entry");
      REQUIRE(filter != nullptr);
      showFailure();
      filter->set_text("true");
      CHECK(dialog.draft().expression == "true");
    }

    CHECK_FALSE(errorLabel->get_visible());
  }

  TEST_CASE("SmartListDialog - a pending filter refresh preserves a later submission error")
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
                                  runtime.workspace().customPresets(),
                                  ao::test::englishMessageCatalog(),
                                  rt::kAllTracksListId,
                                  cache};
    drainGtkEvents();

    auto* const errorLabel = findWidgetByClass<Gtk::Label>(dialog, "ao-layout-error");
    auto* const filter = findWidgetByClass<Gtk::Entry>(dialog, "ao-query-expression-entry");
    auto* const createButton = findButtonByLabel(dialog, "Create");
    REQUIRE(errorLabel != nullptr);
    REQUIRE(filter != nullptr);
    REQUIRE(createButton != nullptr);
    auto& name = requireEntryByPlaceholder(dialog, "List name");
    name.set_text("List draft");
    REQUIRE(dialog.tryBeginSubmission());

    // Edit during the write, then deliver its failure before yielding to the timeout.
    filter->set_text("(");
    dialog.completeSubmission();
    dialog.showError("Create list is busy");
    CHECK(errorLabel->get_text() == "Create list is busy");
    REQUIRE_FALSE(filter->has_css_class("ao-query-invalid"));
    REQUIRE(tryPumpGtkEventsUntil([&] { return filter->has_css_class("ao-query-invalid"); }));
    CHECK_FALSE(createButton->get_sensitive());
    CHECK(errorLabel->get_visible());
    CHECK(errorLabel->get_text().raw().starts_with("Create list is busy\nFilter error:"));

    name.set_text("Renamed draft");
    CHECK(errorLabel->get_visible());
    CHECK(errorLabel->get_text().raw().starts_with("Filter error:"));
  }

  // A list created from the library root, and any top-level list opened for
  // editing, carries parentId kInvalidListId. The source cache rejects that id
  // outright, so the dialog must resolve it to the All Tracks root or the
  // preview never builds and the editor opens showing an acquisition error.
  TEST_CASE("SmartListDialog - root parent previews against All Tracks")
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
                                  runtime.workspace().customPresets(),
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

  TEST_CASE("SmartListDialog - Playlist template exposes a visible tag and chooses Manual Order")
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
                                  runtime.workspace().customPresets(),
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

  TEST_CASE("SmartListDialog - retained presentation callback retires with the dialog")
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
                                    runtime.workspace().customPresets(),
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

  TEST_CASE("SmartListDialog - round-trips a list's assigned custom presentation")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto listId = kInvalidListId;
    auto fixture =
      GtkRuntimeFixture{[&listId](library::MusicLibrary& library)
                        {
                          auto transaction = library::test::writeTransaction(library);
                          auto builder = library::ListBuilder::makeEmpty().name("Custom Presented");
                          listId = ao::test::requireValue(transaction.apply([&builder](library::LibraryWrite& write)
                                                                            { return write.lists().create(builder); }));
                          REQUIRE(transaction.commit());
                        }};
    auto window = Gtk::Window{};
    auto& runtime = fixture.runtime();
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};

    auto preset = rt::CustomTrackPresentationPreset{};
    preset.label = "My Albums View";
    preset.spec.id = "custom-albums-view";
    REQUIRE(runtime.workspace().addCustomPreset(preset));

    auto const optNode = runtime.library().snapshot().listNode(listId);
    REQUIRE(optNode);

    auto dialog = SmartListDialog{window,
                                  runtime.library(),
                                  runtime.views(),
                                  runtime.sources(),
                                  runtime.completion(),
                                  runtime.workspace().customPresets(),
                                  ao::test::englishMessageCatalog(),
                                  kInvalidListId,
                                  cache};

    // The list is assigned the custom presentation, so the editor must open
    // with that presentation selected instead of silently falling to Auto.
    dialog.populate(listId, *optNode, std::optional<std::string>{"custom-albums-view"});
    drainGtkEvents();

    auto const dropDowns = collectAll<Gtk::DropDown>(dialog);
    REQUIRE(dropDowns.size() == 1);
    auto* const dropDown = dropDowns.front();
    auto const stringListPtr = std::dynamic_pointer_cast<Gtk::StringList>(dropDown->get_model());
    REQUIRE(stringListPtr);

    auto const customPosition = static_cast<guint>(rt::builtinTrackPresentationPresets().size()) + 1U;
    CHECK(dropDown->get_selected() == customPosition);
    CHECK(stringListPtr->get_string(customPosition) == "My Albums View");

    // A known custom id appends no unavailable option to the list.
    CHECK(stringListPtr->get_n_items() == customPosition + 1U);

    // The id the editor hands back for saving round-trips the custom id
    // unchanged instead of rewriting it to the Auto recommendation.
    CHECK(dialog.presentationId() == "custom-albums-view");
  }

  TEST_CASE("SmartListDialog - a new list starts as Auto with an absent preference")
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
                                  runtime.workspace().customPresets(),
                                  ao::test::englishMessageCatalog(),
                                  rt::kAllTracksListId,
                                  cache};
    drainGtkEvents();

    // Auto is absence of a preference, so an unsaved editor hands back the
    // empty id instead of today's concrete recommendation.
    CHECK(dialog.presentationId().empty());
  }

  TEST_CASE("SmartListDialog - shows an unknown stored presentation as an explicit unavailable option")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto listId = kInvalidListId;
    auto fixture =
      GtkRuntimeFixture{[&listId](library::MusicLibrary& library)
                        {
                          auto transaction = library::test::writeTransaction(library);
                          auto builder = library::ListBuilder::makeEmpty().name("Unavailable Presented");
                          listId = ao::test::requireValue(transaction.apply([&builder](library::LibraryWrite& write)
                                                                            { return write.lists().create(builder); }));
                          REQUIRE(transaction.commit());
                        }};
    auto window = Gtk::Window{};
    auto& runtime = fixture.runtime();
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};

    auto const optNode = runtime.library().snapshot().listNode(listId);
    REQUIRE(optNode);

    auto dialog = SmartListDialog{window,
                                  runtime.library(),
                                  runtime.views(),
                                  runtime.sources(),
                                  runtime.completion(),
                                  runtime.workspace().customPresets(),
                                  ao::test::englishMessageCatalog(),
                                  kInvalidListId,
                                  cache};

    // The stored id names no live preset, exactly like a preference left over
    // from a deleted custom presentation.
    dialog.populate(listId, *optNode, std::optional<std::string>{"custom-gone"});
    drainGtkEvents();

    auto const dropDowns = collectAll<Gtk::DropDown>(dialog);
    REQUIRE(dropDowns.size() == 1);
    auto* const dropDown = dropDowns.front();
    auto const stringListPtr = std::dynamic_pointer_cast<Gtk::StringList>(dropDown->get_model());
    REQUIRE(stringListPtr);

    auto const unavailablePosition = static_cast<guint>(rt::builtinTrackPresentationPresets().size()) + 1U;
    CHECK(dropDown->get_selected() == unavailablePosition);
    CHECK(stringListPtr->get_string(unavailablePosition) == "Unavailable: custom-gone (Auto)");
    CHECK(stringListPtr->get_n_items() == unavailablePosition + 1U);
    CHECK(dialog.presentationId() == "custom-gone");

    // A rename keeps the unavailable selection, so the save still carries the
    // original opaque id.
    auto* nameEntry = static_cast<Gtk::Entry*>(nullptr);

    for (auto* const entry : collectAll<Gtk::Entry>(dialog))
    {
      if (entry->get_placeholder_text() == "List name")
      {
        nameEntry = entry;
        break;
      }
    }

    REQUIRE(nameEntry != nullptr);
    nameEntry->set_text("Renamed");
    drainGtkEvents();
    CHECK(dialog.presentationId() == "custom-gone");

    // Intentionally switching to Auto clears the preference to absence.
    dropDown->set_selected(0U);
    drainGtkEvents();
    CHECK(dialog.presentationId().empty());

    // The unavailable row stays available; reselecting it restores the id.
    dropDown->set_selected(unavailablePosition);
    drainGtkEvents();
    CHECK(dialog.presentationId() == "custom-gone");

    // A concrete builtin replaces the preference with that exact id.
    dropDown->set_selected(2U);
    drainGtkEvents();
    CHECK(dialog.presentationId() == rt::kListOrderTrackPresentationId);
  }

  TEST_CASE("SmartListDialog - repopulating replaces the unavailable option without stale rows")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto listId = kInvalidListId;
    auto fixture =
      GtkRuntimeFixture{[&listId](library::MusicLibrary& library)
                        {
                          auto transaction = library::test::writeTransaction(library);
                          auto builder = library::ListBuilder::makeEmpty().name("Repopulated");
                          listId = ao::test::requireValue(transaction.apply([&builder](library::LibraryWrite& write)
                                                                            { return write.lists().create(builder); }));
                          REQUIRE(transaction.commit());
                        }};
    auto window = Gtk::Window{};
    auto& runtime = fixture.runtime();
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};

    auto preset = rt::CustomTrackPresentationPreset{};
    preset.label = "Labeled View";
    preset.spec.id = "custom-labeled";
    REQUIRE(runtime.workspace().addCustomPreset(preset));

    auto const optNode = runtime.library().snapshot().listNode(listId);
    REQUIRE(optNode);

    auto dialog = SmartListDialog{window,
                                  runtime.library(),
                                  runtime.views(),
                                  runtime.sources(),
                                  runtime.completion(),
                                  runtime.workspace().customPresets(),
                                  ao::test::englishMessageCatalog(),
                                  kInvalidListId,
                                  cache};

    auto const dropDowns = collectAll<Gtk::DropDown>(dialog);
    REQUIRE(dropDowns.size() == 1);
    auto* const dropDown = dropDowns.front();
    auto const stringListPtr = std::dynamic_pointer_cast<Gtk::StringList>(dropDown->get_model());
    REQUIRE(stringListPtr);

    auto const baseRowCount = static_cast<guint>(rt::builtinTrackPresentationPresets().size()) + 2U;
    auto const unavailablePosition = baseRowCount;

    dialog.populate(listId, *optNode, std::optional<std::string>{"custom-gone"});
    drainGtkEvents();
    CHECK(stringListPtr->get_n_items() == baseRowCount + 1U);
    CHECK(dropDown->get_selected() == unavailablePosition);
    CHECK(stringListPtr->get_string(unavailablePosition) == "Unavailable: custom-gone (Auto)");

    // A known custom id leaves no unavailable row behind.
    dialog.populate(listId, *optNode, std::optional<std::string>{"custom-labeled"});
    drainGtkEvents();
    CHECK(stringListPtr->get_n_items() == baseRowCount);
    CHECK(dropDown->get_selected() == unavailablePosition - 1U);
    CHECK(dialog.presentationId() == "custom-labeled");

    // A different unknown id recreates exactly one unavailable row.
    dialog.populate(listId, *optNode, std::optional<std::string>{"custom-other"});
    drainGtkEvents();
    CHECK(stringListPtr->get_n_items() == baseRowCount + 1U);
    CHECK(dropDown->get_selected() == unavailablePosition);
    CHECK(stringListPtr->get_string(unavailablePosition) == "Unavailable: custom-other (Auto)");
    CHECK(dialog.presentationId() == "custom-other");

    // Absence and an explicitly empty id both count as Auto, not unavailable.
    dialog.populate(listId, *optNode, std::nullopt);
    drainGtkEvents();
    CHECK(stringListPtr->get_n_items() == baseRowCount);
    CHECK(dropDown->get_selected() == 0U);
    CHECK(dialog.presentationId().empty());

    dialog.populate(listId, *optNode, std::optional<std::string>{""});
    drainGtkEvents();
    CHECK(stringListPtr->get_n_items() == baseRowCount);
    CHECK(dropDown->get_selected() == 0U);
    CHECK(dialog.presentationId().empty());
  }

  TEST_CASE("SmartListDialog - a custom presentation without a label falls back to its raw id")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto window = Gtk::Window{};
    auto& runtime = fixture.runtime();
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};

    auto preset = rt::CustomTrackPresentationPreset{};
    preset.label = "";
    preset.spec.id = "custom-unnamed";
    REQUIRE(runtime.workspace().addCustomPreset(preset));

    auto dialog = SmartListDialog{window,
                                  runtime.library(),
                                  runtime.views(),
                                  runtime.sources(),
                                  runtime.completion(),
                                  runtime.workspace().customPresets(),
                                  ao::test::englishMessageCatalog(),
                                  rt::kAllTracksListId,
                                  cache};
    drainGtkEvents();

    auto const dropDowns = collectAll<Gtk::DropDown>(dialog);
    REQUIRE(dropDowns.size() == 1);
    auto* const dropDown = dropDowns.front();
    auto const stringListPtr = std::dynamic_pointer_cast<Gtk::StringList>(dropDown->get_model());
    REQUIRE(stringListPtr);

    auto const customPosition = static_cast<guint>(rt::builtinTrackPresentationPresets().size()) + 1U;
    CHECK(stringListPtr->get_string(customPosition) == "custom-unnamed");

    dropDown->set_selected(customPosition);
    drainGtkEvents();
    CHECK(dialog.presentationId() == "custom-unnamed");
  }

  TEST_CASE("SmartListDialog - the selected presentation option ellipsizes and keeps its full text reachable")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto listId = kInvalidListId;
    auto fixture =
      GtkRuntimeFixture{[&listId](library::MusicLibrary& library)
                        {
                          auto transaction = library::test::writeTransaction(library);
                          auto builder = library::ListBuilder::makeEmpty().name("Bounded Selection");
                          listId = ao::test::requireValue(transaction.apply([&builder](library::LibraryWrite& write)
                                                                            { return write.lists().create(builder); }));
                          REQUIRE(transaction.commit());
                        }};
    auto window = Gtk::Window{};
    auto& runtime = fixture.runtime();
    auto cache = TrackRowCache{runtime.library(), ao::test::englishMessageCatalog()};

    auto const optNode = runtime.library().snapshot().listNode(listId);
    REQUIRE(optNode);

    auto dialog = SmartListDialog{window,
                                  runtime.library(),
                                  runtime.views(),
                                  runtime.sources(),
                                  runtime.completion(),
                                  runtime.workspace().customPresets(),
                                  ao::test::englishMessageCatalog(),
                                  kInvalidListId,
                                  cache};

    auto const unavailableId = std::string{"custom-unavailable-presentation-id-with-a-long-name"};
    dialog.populate(listId, *optNode, std::optional<std::string>{unavailableId});
    drainGtkEvents();

    auto const selectedText = std::string{"Unavailable: "} + unavailableId + " (Auto)";
    auto const dropDowns = collectAll<Gtk::DropDown>(dialog);
    REQUIRE(dropDowns.size() == 1);
    auto* const dropDown = dropDowns.front();

    // The selected item renders through the bounded factory: its label can
    // give up natural width in the fixed configuration pane instead of
    // squeezing the field label, and the full text stays reachable as tooltip
    // and accessible label.
    auto* const selectedLabel = findLabelByText(dialog, selectedText);
    REQUIRE(selectedLabel != nullptr);
    CHECK(selectedLabel->get_ellipsize() == Pango::EllipsizeMode::END);
    CHECK(selectedLabel->get_tooltip_text() == Glib::ustring{selectedText});
    CHECK(hasAccessibleLabel(*selectedLabel, selectedText));

    // Changing the selection rebinds the selected item with the fresh
    // option's full text, so the bounded rendering follows the user's choice.
    dropDown->set_selected(0U);
    drainGtkEvents();
    CHECK(selectedLabel->get_text() == "Auto");
    CHECK(selectedLabel->get_tooltip_text() == "Auto");
    CHECK(hasAccessibleLabel(*selectedLabel, "Auto"));
  }

  TEST_CASE("SmartListDialog - popup rows carry a trailing check on the selected option")
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
                                  runtime.workspace().customPresets(),
                                  ao::test::englishMessageCatalog(),
                                  rt::kAllTracksListId,
                                  cache};

    // The popup factory is public on the dropdown: attaching it to a
    // standalone selection list witnesses the row content, including the
    // check binding, without opening the dropdown's private popover.
    auto const dropDowns = collectAll<Gtk::DropDown>(dialog);
    REQUIRE(dropDowns.size() == 1);
    auto const popupFactoryPtr = dropDowns.front()->get_list_factory();
    REQUIRE(popupFactoryPtr != nullptr);

    auto const unavailableText = std::string{"Unavailable: custom-gone (Auto)"};
    auto stringsPtr = Gtk::StringList::create({Glib::ustring{unavailableText}, "Auto"});
    auto selectionPtr = Gtk::SingleSelection::create(stringsPtr);
    auto listView = Gtk::ListView{selectionPtr};
    listView.set_factory(popupFactoryPtr);

    auto host = GtkWindowFixture{};
    host.window().set_default_size(400, 200);
    host.mount(listView);
    host.present();
    REQUIRE(listView.get_mapped());

    // Returns the trailing check image of the row whose label is @p rowText.
    auto const rowCheck = [&](char const* const rowText) -> Gtk::Image*
    {
      for (auto* const label : collectAll<Gtk::Label>(listView))
      {
        if (label->get_text() == rowText)
        {
          return dynamic_cast<Gtk::Image*>(label->get_next_sibling());
        }
      }

      return nullptr;
    };

    REQUIRE(rowCheck("Unavailable: custom-gone (Auto)") != nullptr);
    REQUIRE(rowCheck("Auto") != nullptr);

    // The list starts on the unavailable row: its check is visible from the
    // synchronized binding and the unselected row's check is hidden.
    CHECK(rowCheck("Unavailable: custom-gone (Auto)")->get_visible());
    CHECK_FALSE(rowCheck("Auto")->get_visible());

    // Switching the selection moves the check to the Auto row.
    selectionPtr->set_selected(1U);
    drainGtkEvents();
    CHECK_FALSE(rowCheck("Unavailable: custom-gone (Auto)")->get_visible());
    CHECK(rowCheck("Auto")->get_visible());
  }
} // namespace ao::gtk::test
