// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tag/TagEditController.h"

#include "../../TestUtils.h"
#include "app/ThemeCoordinator.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/Type.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/NotificationService.h>
#include <ao/rt/StateTypes.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryWriter.h>

#include <catch2/catch_test_macros.hpp>
#include <giomm/simpleactiongroup.h>
#include <gtkmm/window.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace ao::gtk::test
{
  namespace
  {
    TrackId createTrack(library::MusicLibrary& library, std::string const& title)
    {
      auto txn = library.writeTransaction();
      auto writer = library.tracks().writer(txn);
      auto builder = library::TrackBuilder::createNew();
      builder.metadata().title(title);
      auto serializeResult = builder.serialize(txn, library.dictionary(), library.resources());
      REQUIRE(serializeResult);
      auto const [hotData, coldData] = *serializeResult;
      auto const trackId = ao::test::requireValue(writer.createHotCold(hotData, coldData)).first;
      txn.commit();
      return trackId;
    }

    bool trackHasTag(library::MusicLibrary& library, TrackId trackId, std::string const& expectedTag)
    {
      auto txn = library.readTransaction();
      auto reader = library.tracks().reader(txn);
      auto const optView = reader.get(trackId);

      if (!optView)
      {
        return false;
      }

      auto const& dictionary = library.dictionary();

      return std::ranges::any_of(
        optView->tags(), [&](auto const tagId) { return dictionary.getOrDefault(tagId) == expectedTag; });
    }
  } // namespace

  TEST_CASE("TagEditController - smoke test", "[gtk][tag][controller]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto window = Gtk::Window{};

    auto themeController = ThemeCoordinator{};
    std::int32_t mutationCallbacks = 0;
    auto callbacks = TagEditController::Callbacks{.onTagsMutated = [&mutationCallbacks] { ++mutationCallbacks; }};

    auto controller = TagEditController{window, fixture.runtime(), std::move(callbacks), themeController};

    SECTION("registers tag actions")
    {
      auto groupPtr = Gio::SimpleActionGroup::create();
      controller.addActionsTo(*groupPtr);

      auto addActionPtr = std::dynamic_pointer_cast<Gio::SimpleAction>(groupPtr->lookup_action("track-tag-add"));
      REQUIRE(addActionPtr);

      addActionPtr->activate(Glib::Variant<Glib::ustring>::create("ActionTag"));
      drainGtkEvents();
      CHECK(mutationCallbacks == 0);
    }

    SECTION("submitTagChanges applies tags and reports the mutation")
    {
      auto& library = fixture.runtime().musicLibrary();
      auto const firstTrackId = createTrack(library, "Controller Target 1");
      auto const secondTrackId = createTrack(library, "Controller Target 2");
      auto const selection =
        TrackSelectionContext{.listId = rt::kAllTracksListId, .selectedIds = {firstTrackId, secondTrackId}};
      auto const tagsToAdd = std::array<std::string, 1>{"ControllerTag"};

      controller.submitTagChanges(selection, tagsToAdd, std::span<std::string const>{});

      CHECK(mutationCallbacks == 1);
      CHECK(trackHasTag(library, firstTrackId, "ControllerTag"));
      CHECK(trackHasTag(library, secondTrackId, "ControllerTag"));

      auto const feed = fixture.runtime().notifications().feed();
      REQUIRE(feed.entries.size() == 1);
      CHECK(feed.entries.back().severity == rt::NotificationSeverity::Info);
      CHECK(feed.entries.back().message == "Tags added 1 for 2 tracks");
    }

    SECTION("submitTagChanges ignores empty tag deltas")
    {
      auto const selection = TrackSelectionContext{.listId = rt::kAllTracksListId, .selectedIds = {TrackId{42}}};

      controller.submitTagChanges(selection, std::span<std::string const>{}, std::span<std::string const>{});

      CHECK(mutationCallbacks == 0);
      CHECK(fixture.runtime().notifications().feed().entries.empty());
    }

    SECTION("submitTagChanges can add and remove tags in one mutation")
    {
      auto& library = fixture.runtime().musicLibrary();
      auto const trackId = createTrack(library, "Mixed Tag Target");
      auto const existingTags = std::array<std::string, 1>{"OldTag"};
      REQUIRE_FALSE(
        fixture.runtime().library().writer().editTags(std::array{trackId}, existingTags, {}).mutatedIds.empty());

      auto const selection = TrackSelectionContext{.listId = rt::kAllTracksListId, .selectedIds = {trackId}};
      auto const tagsToAdd = std::array<std::string, 1>{"NewTag"};
      auto const tagsToRemove = std::array<std::string, 1>{"OldTag"};

      controller.submitTagChanges(selection, tagsToAdd, tagsToRemove);

      CHECK(mutationCallbacks == 1);
      CHECK(trackHasTag(library, trackId, "NewTag"));
      CHECK_FALSE(trackHasTag(library, trackId, "OldTag"));

      auto const feed = fixture.runtime().notifications().feed();
      REQUIRE(feed.entries.size() == 1);
      CHECK(feed.entries.back().severity == rt::NotificationSeverity::Info);
      CHECK(feed.entries.back().message == "Tags added 1 and removed 1 for 1 track");
    }
  }
} // namespace ao::gtk::test
