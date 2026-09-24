// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "track/TrackRowObject.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include <ao/AudioCodec.h>
#include <ao/CoreIds.h>
#include <ao/rt/TrackField.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

namespace ao::gtk::test
{
  TEST_CASE("TrackRowObject - mutations refresh computed and text-backed display values", "[gtk][unit][track]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto const rowPtr = TrackRowObject::create(TrackId{1}, ao::test::englishMessageCatalog());
    REQUIRE(rowPtr);
    rowPtr->populate("Track 1",
                     "Artist 1",
                     "Album 1",
                     "",
                     "Genre 1",
                     "",
                     "",
                     "",
                     "",
                     "",
                     "",
                     "",
                     "track-1.flac",
                     std::chrono::minutes{3},
                     2021,
                     1,
                     0,
                     1,
                     0,
                     0,
                     0,
                     44100,
                     2,
                     16,
                     AudioCodec::Unknown,
                     320000,
                     0,
                     0);

    CHECK(rowPtr->trySetStringField(rt::TrackField::Artist, "New Artist"));
    CHECK(rowPtr->fieldText(rt::TrackField::Artist) == "New Artist");
    CHECK_FALSE(rowPtr->trySetStringField(rt::TrackField::Duration, "Failed"));
    CHECK(rowPtr->duration() == std::chrono::minutes{3});

    rowPtr->setYear(2025);
    rowPtr->setDiscNumber(2);
    rowPtr->setDiscTotal(3);
    rowPtr->setTrackNumber(4);
    rowPtr->setTrackTotal(10);
    CHECK(rowPtr->year() == 2025);
    CHECK(rowPtr->discNumber() == 2);
    CHECK(rowPtr->discTotal() == 3);
    CHECK(rowPtr->trackNumber() == 4);
    CHECK(rowPtr->trackTotal() == 10);

    REQUIRE(rowPtr->displayText(rt::TrackField::Year) != nullptr);
    CHECK(*rowPtr->displayText(rt::TrackField::Year) == "2025");
    rowPtr->setYear(1999);
    REQUIRE(rowPtr->displayText(rt::TrackField::Year) != nullptr);
    CHECK(*rowPtr->displayText(rt::TrackField::Year) == "1999");

    REQUIRE(rowPtr->displayText(rt::TrackField::TrackNumber) != nullptr);
    CHECK(*rowPtr->displayText(rt::TrackField::TrackNumber) == "4");
    REQUIRE(rowPtr->displayText(rt::TrackField::Artist) != nullptr);
    CHECK(rowPtr->displayText(rt::TrackField::Artist) == rowPtr->stringField(rt::TrackField::Artist));
    CHECK(*rowPtr->displayText(rt::TrackField::Artist) == "New Artist");
    CHECK(rowPtr->displayText(static_cast<rt::TrackField>(255)) == nullptr);
  }
} // namespace ao::gtk::test
