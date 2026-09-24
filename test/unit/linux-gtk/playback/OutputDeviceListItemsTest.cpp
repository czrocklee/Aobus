// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/OutputDeviceListItems.h"

#include "test/unit/linux-gtk/GtkApplicationTestSupport.h"
#include <ao/audio/Device.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::gtk::test
{
  TEST_CASE("OutputBackendItem - retains backend identity and display name", "[gtk][unit][playback][output]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto itemPtr = OutputBackendItem::create(audio::BackendId{"alsa"}, "ALSA");
    REQUIRE(itemPtr);
    CHECK(itemPtr->id() == audio::BackendId{"alsa"});
    CHECK(itemPtr->name() == "ALSA");
  }

  TEST_CASE("OutputDeviceItem - retains device identity and mutable active state", "[gtk][unit][playback][output]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto device = audio::Device{.id = audio::DeviceId{"hw:0,0"}, .displayName = "Default", .description = "Hardware"};
    auto itemPtr = OutputDeviceItem::create(audio::BackendId{"alsa"}, device, audio::ProfileId{"stereo"}, "E");
    REQUIRE(itemPtr);

    CHECK(itemPtr->backendId() == audio::BackendId{"alsa"});
    CHECK(itemPtr->id() == audio::DeviceId{"hw:0,0"});
    CHECK(itemPtr->profileId() == audio::ProfileId{"stereo"});
    CHECK(itemPtr->name() == "Default");
    CHECK(itemPtr->description() == "Hardware");
    CHECK(itemPtr->badge() == "E");
    CHECK(itemPtr->matches(audio::BackendId{"alsa"}, audio::DeviceId{"hw:0,0"}, audio::ProfileId{"stereo"}));
    CHECK_FALSE(itemPtr->matches(audio::BackendId{"other"}, audio::DeviceId{"hw:0,0"}, audio::ProfileId{"stereo"}));
    CHECK_FALSE(itemPtr->matches(audio::BackendId{"alsa"}, audio::DeviceId{"other"}, audio::ProfileId{"stereo"}));
    CHECK_FALSE(itemPtr->matches(audio::BackendId{"alsa"}, audio::DeviceId{"hw:0,0"}, audio::ProfileId{"other"}));
    CHECK_FALSE(itemPtr->isActive());
    itemPtr->setActive(true);
    CHECK(itemPtr->isActive());
    itemPtr->setActive(false);
    CHECK_FALSE(itemPtr->isActive());
  }
} // namespace ao::gtk::test
