// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/OutputListItems.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/audio/Backend.h>

#include <catch2/catch_test_macros.hpp>

namespace ao::gtk::test
{
  TEST_CASE("OutputListItems expose GTK item identity and active state", "[gtk][unit][playback][output]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();

    SECTION("BackendItem")
    {
      auto itemPtr = BackendItem::create(audio::BackendId{"alsa"}, "ALSA");
      REQUIRE(itemPtr);
    }

    SECTION("DeviceItem")
    {
      auto device = audio::Device{.id = audio::DeviceId{"hw:0,0"}, .displayName = "Default", .description = "Hardware"};
      auto itemPtr = DeviceItem::create(audio::BackendId{"alsa"}, device, audio::ProfileId{"stereo"});
      REQUIRE(itemPtr);

      itemPtr->setActive(true);
      CHECK(itemPtr->active() == true);
    }
  }
} // namespace ao::gtk::test
