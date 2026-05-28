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
      auto item = BackendItem::create(audio::BackendId{"alsa"}, "ALSA");
      REQUIRE(item);
      CHECK(item->id() == audio::BackendId{"alsa"});
      CHECK(item->name() == "ALSA");
    }

    SECTION("DeviceItem")
    {
      auto device = audio::Device{.id = audio::DeviceId{"hw:0,0"}, .displayName = "Default", .description = "Hardware"};
      auto item = DeviceItem::create(audio::BackendId{"alsa"}, device, audio::ProfileId{"stereo"});
      REQUIRE(item);
      CHECK(item->backendId() == audio::BackendId{"alsa"});
      CHECK(item->id() == audio::DeviceId{"hw:0,0"});
      CHECK(item->profileId() == audio::ProfileId{"stereo"});
      CHECK(item->name() == "Default");
      CHECK(item->description() == "Hardware");
      CHECK(item->active() == false);

      item->setActive(true);
      CHECK(item->active() == true);

      CHECK(item->matches(audio::BackendId{"alsa"}, audio::DeviceId{"hw:0,0"}, audio::ProfileId{"stereo"}));
      CHECK_FALSE(item->matches(audio::BackendId{"pipewire"}, audio::DeviceId{"hw:0,0"}, audio::ProfileId{"stereo"}));
    }
  }
} // namespace ao::gtk::test
