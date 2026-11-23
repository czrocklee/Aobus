// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "track/TrackRowObject.h"

#include "test/MessageCatalogTestSupport.h"
#include "test/linux-gtk/GtkApplicationTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/RecordingDate.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/TrackRow.h>

#include <catch2/catch_test_macros.hpp>
#include <sigc++/scoped_connection.h>

#include <chrono>
#include <string>
#include <vector>

namespace ao::gtk::test
{
  TEST_CASE("TrackRowObject - same-track snapshots replace text and reset computed display values")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto const id = TrackId{1};
    auto const rowPtr = TrackRowObject::create(id, ao::test::englishMessageCatalog());
    REQUIRE(rowPtr);
    rowPtr->populate(rt::TrackRow{.id = id,
                                  .artist = "Original artist",
                                  .album = "Original album",
                                  .duration = std::chrono::minutes{3},
                                  .year = 2021,
                                  .recordingDate = {.year = 1955, .month = 6, .day = 7}});

    auto const* const artist = rowPtr->displayText(rt::TrackField::Artist);
    REQUIRE(artist != nullptr);
    CHECK(artist == rowPtr->stringField(rt::TrackField::Artist));
    CHECK(*artist == "Original artist");
    CHECK(rowPtr->fieldText(rt::TrackField::Year) == "2021");
    CHECK(rowPtr->fieldText(rt::TrackField::Duration) == "3:00");
    CHECK(rowPtr->fieldText(rt::TrackField::RecordingDate) == "1955-06-07");

    // Subscribers must see refreshed computed text during the notification itself.
    auto publishedDates = std::vector<std::string>{};
    auto const fieldsChangedConnection = sigc::scoped_connection{rowPtr->signalFieldsChanged().connect(
      [&rowPtr, &publishedDates]
      { publishedDates.push_back(rowPtr->fieldText(rt::TrackField::RecordingDate).raw()); })};
    rowPtr->populate(rt::TrackRow{.id = id,
                                  .artist = "Updated artist",
                                  .duration = std::chrono::minutes{4},
                                  .year = 2025,
                                  .recordingDate = {.year = 1955, .month = 6}});

    CHECK(rowPtr->trackId() == id);
    CHECK(rowPtr->displayText(rt::TrackField::Artist) == artist);
    CHECK(*artist == "Updated artist");
    CHECK(rowPtr->fieldText(rt::TrackField::Album).empty());
    CHECK(rowPtr->fieldText(rt::TrackField::Year) == "2025");
    CHECK(rowPtr->fieldText(rt::TrackField::Duration) == "4:00");
    CHECK(rowPtr->recordingDate() == library::RecordingDate{.year = 1955, .month = 6});

    rowPtr->populate(rt::TrackRow{.id = id, .recordingDate = {.year = 1981}});
    rowPtr->populate(rt::TrackRow{.id = id});
    CHECK(publishedDates == std::vector<std::string>{"1955-06", "1981", ""});
    CHECK(rowPtr->displayText(static_cast<rt::TrackField>(255)) == nullptr);
  }
} // namespace ao::gtk::test
