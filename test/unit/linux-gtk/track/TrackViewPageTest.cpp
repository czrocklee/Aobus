// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/TrackViewPage.h"

#include "../../TestUtils.h"
#include "image/ImageCache.h"
#include "image/ThumbnailLoader.h"
#include "layout/LayoutConstants.h"
#include "test/unit/linux-gtk/GtkTestSupport.h"
#include "track/TrackListModel.h"
#include "track/TrackRowCache.h"
#include <ao/Type.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/TrackBuilder.h>
#include <ao/library/TrackStore.h>
#include <ao/lmdb/Transaction.h>
#include <ao/rt/CorePrimitives.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackPresentation.h>
#include <ao/rt/projection/TrackListProjection.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/uimodel/track/TrackPresentationViewModel.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/enums.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    class MutableTrackSource final : public rt::TrackSource
    {
    public:
      void addInitial(TrackId id) { _ids.push_back(id); }
      std::size_t size() const override { return _ids.size(); }
      TrackId trackIdAt(std::size_t index) const override { return _ids.at(index); }
      std::optional<std::size_t> indexOf(TrackId id) const override
      {
        auto const it = std::ranges::find(_ids, id);

        if (it == _ids.end())
        {
          return std::nullopt;
        }

        return static_cast<std::size_t>(std::ranges::distance(_ids.begin(), it));
      }

    private:
      std::vector<TrackId> _ids;
    };

    TrackId addAlbumTrack(library::MusicLibrary& library, std::string const& album)
    {
      auto txn = library.writeTransaction();
      auto writer = library.tracks().writer(txn);

      auto builder = library::TrackBuilder::createNew();
      builder.metadata().title("Track").artist("Artist").album(album).albumArtist("Album Artist").trackNumber(1);
      builder.property().uri("/tmp/track.flac").duration(std::chrono::minutes{3});

      auto const [hotData, coldData] =
        ao::test::requireValue(builder.serialize(txn, library.dictionary(), library.resources()));
      auto const [id, _] = ao::test::requireValue(writer.createHotCold(hotData, coldData));
      txn.commit();
      return id;
    }
  } // namespace

  TEST_CASE("TrackViewPage - initialization", "[gtk][track][page]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& runtime = fixture.runtime();
    auto cache = TrackRowCache{runtime.library()};
    auto imageCache = ImageCache{200};
    auto thumbnailLoader = ThumbnailLoader{runtime.library(), imageCache, runtime.async()};
    auto window = Gtk::Window{};

    auto modelPtr = TrackListModel::create(cache);
    auto presentationStore = uimodel::track::TrackPresentationViewModel{runtime.workspace()};

    auto page = TrackViewPage{rt::kAllTracksListId, modelPtr, presentationStore, runtime, thumbnailLoader};
    window.set_child(page);

    SECTION("initial state")
    {
      CHECK(page.listId() == rt::kAllTracksListId);
      CHECK(page.projection() == nullptr);
    }

    SECTION("status message shows then hides the status label")
    {
      page.setStatusMessage("Loading...");
      auto* const label = findLabelByText(page, "Loading...");
      REQUIRE(label != nullptr);
      CHECK(label->get_visible());

      page.clearStatusMessage();
      CHECK_FALSE(label->get_visible());
    }

    SECTION("album grouped section header reserves a fixed cover slot")
    {
      auto source = MutableTrackSource{};
      source.addInitial(addAlbumTrack(runtime.musicLibrary(), "Album"));

      auto projectionPtr = std::make_shared<rt::TrackListProjection>(rt::ViewId{1}, source, runtime.musicLibrary());
      auto presentation = rt::TrackPresentationSpec{.groupBy = rt::TrackGroupKey::Album};
      projectionPtr->setPresentation(presentation);
      modelPtr->bindProjection(projectionPtr);
      page.applyPresentation(projectionPtr->presentation());

      window.set_default_size(600, 320);
      window.set_visible(true);
      drainGtkEvents();

      auto* const coverSlot = findWidgetByClass<Gtk::Widget>(page, "ao-track-section-cover");
      REQUIRE(coverSlot != nullptr);
      CHECK(coverSlot->get_visible());

      std::int32_t minSize = {};
      std::int32_t natSizeHoriz = {};
      std::int32_t natSizeVert = {};
      std::int32_t minBaseline = {};
      std::int32_t natBaseline = {};
      coverSlot->measure(Gtk::Orientation::HORIZONTAL, -1, minSize, natSizeHoriz, minBaseline, natBaseline);
      coverSlot->measure(Gtk::Orientation::VERTICAL, -1, minSize, natSizeVert, minBaseline, natBaseline);

      CHECK(natSizeHoriz == natSizeVert);
      CHECK(natSizeHoriz >= layout::kSectionCoverLogicalSize);
      CHECK(natSizeHoriz <= layout::kSectionCoverLogicalSize + 2);
    }
  }
} // namespace ao::gtk::test
