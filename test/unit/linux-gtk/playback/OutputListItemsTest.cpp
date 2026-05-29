// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/OutputListItems.h"

#include <ao/audio/Backend.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::gtk::test
{
  TEST_CASE("OutputListItems - data models", "[gtk][playback][output]")
  {
    SECTION("BackendItem")
    {
      auto itemPtr = BackendItem::create(audio::BackendId{"alsa"}, "ALSA");
      REQUIRE(itemPtr);
      CHECK(itemPtr->id() == audio::BackendId{"alsa"});
      CHECK(itemPtr->name() == "ALSA");
    }

    SECTION("DeviceItem")
    {
      auto device = audio::Device{.id = audio::DeviceId{"hw:0,0"}, .displayName = "Default", .description = "Hardware"};
      auto itemPtr = DeviceItem::create(audio::BackendId{"alsa"}, device, audio::ProfileId{"stereo"});
      REQUIRE(itemPtr);
      CHECK(itemPtr->backendId() == audio::BackendId{"alsa"});
      CHECK(itemPtr->id() == audio::DeviceId{"hw:0,0"});
      CHECK(itemPtr->profileId() == audio::ProfileId{"stereo"});
      CHECK(itemPtr->name() == "Default");
      CHECK(itemPtr->description() == "Hardware");
      CHECK(itemPtr->active() == false);

      itemPtr->setActive(true);
      CHECK(itemPtr->active() == true);

      CHECK(itemPtr->matches(audio::BackendId{"alsa"}, audio::DeviceId{"hw:0,0"}, audio::ProfileId{"stereo"}));
      CHECK_FALSE(
        itemPtr->matches(audio::BackendId{"pipewire"}, audio::DeviceId{"hw:0,0"}, audio::ProfileId{"stereo"}));
    }
  }
} // namespace ao::gtk::test
