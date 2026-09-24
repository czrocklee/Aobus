// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/OutputDevicePanel.h"

#include "test/unit/MessageCatalogTestSupport.h"
#include "test/unit/tui/KeymapTestSupport.h"
#include "test/unit/tui/RenderTestSupport.h"
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/color.hpp>

#include <cstdint>
#include <format>
#include <utility>
#include <vector>

namespace ao::tui::test
{
  namespace
  {
    ftxui::Element englishOutputDevicePanel(uimodel::OutputDeviceViewState const& view,
                                            std::int32_t const selectedRow,
                                            std::vector<OutputDeviceRowHitRegion>* const rowHitRegions = nullptr,
                                            std::int32_t const columns = 0)
    {
      return outputDevicePanel(
        ao::test::englishMessageCatalog(), view, selectedRow, defaultKeymapPlan(), rowHitRegions, columns);
    }

    std::int32_t englishOutputDevicePanelColumns(uimodel::OutputDeviceViewState const& view,
                                                 std::int32_t const terminalColumns)
    {
      return outputDevicePanelColumns(ao::test::englishMessageCatalog(), view, defaultKeymapPlan(), terminalColumns);
    }
  } // namespace

  TEST_CASE("OutputDevicePanel - renders grouped selectable rows", "[tui][unit][output]")
  {
    auto rowHitRegions = std::vector<OutputDeviceRowHitRegion>{};
    auto const view = uimodel::OutputDeviceViewState{
      .rows =
        std::vector{
          uimodel::OutputDeviceRow{
            .kind = uimodel::OutputDeviceRow::Kind::BackendHeader,
            .backendId = audio::BackendId{"pipewire"},
            .title = "PipeWire",
          },
          uimodel::OutputDeviceRow{
            .kind = uimodel::OutputDeviceRow::Kind::DeviceProfile,
            .backendId = audio::BackendId{"pipewire"},
            .deviceId = audio::DeviceId{"studio"},
            .profileId = audio::kProfileShared,
            .title = "Studio DAC",
            .description = "USB interface",
            .isActive = true,
          },
          uimodel::OutputDeviceRow{
            .kind = uimodel::OutputDeviceRow::Kind::DeviceProfile,
            .backendId = audio::BackendId{"pipewire"},
            .deviceId = audio::DeviceId{"studio"},
            .profileId = audio::kProfileExclusive,
            .title = "Studio DAC",
            .isExclusive = true,
          },
        },
      .outputBackendSummary = "PW",
      .outputDeviceStatus = "PipeWire: Studio DAC",
      .hasActiveOutputDevice = true,
    };

    auto const text = renderText(englishOutputDevicePanel(view, 2, &rowHitRegions), 96);

    CHECK(text.contains("Output Devices"));
    CHECK(text.contains("PipeWire"));
    CHECK(text.contains("Studio DAC"));
    CHECK(text.contains("USB interface"));
    CHECK(text.contains("PipeWire: Studio DAC"));
    CHECK(text.contains("Enter select"));
    REQUIRE(rowHitRegions.size() == 2);
    CHECK(rowHitRegions[0].rowIndex == 1);
    CHECK(rowHitRegions[0].backendId == audio::BackendId{"pipewire"});
    CHECK(rowHitRegions[0].deviceId == audio::DeviceId{"studio"});
    CHECK(rowHitRegions[0].profileId == audio::kProfileShared);
    CHECK(rowHitRegions[1].rowIndex == 2);
    CHECK(rowHitRegions[1].profileId == audio::kProfileExclusive);
  }

  TEST_CASE("OutputDevicePanel - empty state uses the selected locale", "[tui][unit][output][localization]")
  {
    auto const german = ao::test::messageCatalog("de-AT");
    auto const view = uimodel::OutputDeviceViewState{};
    auto const text = renderText(outputDevicePanel(german, view, 0, defaultKeymapPlan()), 96);

    CHECK(text.contains("Ausgabegeräte"));
    CHECK(text.contains("Keine Ausgabegeräte gefunden"));
    CHECK(text.contains("Kein Ausgabegerät ausgewählt"));
  }

  TEST_CASE("OutputDevicePanel - width follows content and terminal bounds", "[tui][unit][output]")
  {
    auto view = uimodel::OutputDeviceViewState{
      .rows =
        std::vector{
          uimodel::OutputDeviceRow{
            .kind = uimodel::OutputDeviceRow::Kind::DeviceProfile,
            .backendId = audio::BackendId{"pipewire"},
            .deviceId = audio::DeviceId{"studio"},
            .profileId = audio::kProfileShared,
            .title = "Studio DAC",
          },
        },
      .outputBackendSummary = "PW",
      .outputDeviceStatus = "PipeWire: Studio DAC",
    };
    auto const narrowColumns = englishOutputDevicePanelColumns(view, 120);

    view.rows[0].description = "USB interface with a very long ALSA/PipeWire profile identifier";

    CHECK(englishOutputDevicePanelColumns(view, 120) > narrowColumns);
    CHECK(englishOutputDevicePanelColumns(view, 40) == 40);
  }

  TEST_CASE("OutputDevicePanel - frames long device lists", "[tui][unit][output]")
  {
    auto rows = std::vector{
      uimodel::OutputDeviceRow{
        .kind = uimodel::OutputDeviceRow::Kind::BackendHeader,
        .backendId = audio::BackendId{"pipewire"},
        .title = "PipeWire",
      },
    };

    for (std::int32_t index = 0; index < 20; ++index)
    {
      rows.push_back(uimodel::OutputDeviceRow{
        .kind = uimodel::OutputDeviceRow::Kind::DeviceProfile,
        .backendId = audio::BackendId{"pipewire"},
        .deviceId = audio::DeviceId{std::format("device-{}", index)},
        .profileId = audio::kProfileShared,
        .title = std::format("Device {}", index),
        .description = "alsa_output.usb-Sonata_Sonata_BHD_Pro_Sonata_BHD_Pro_very_long_identifier",
        .isActive = index == 0,
      });
    }

    auto const view = uimodel::OutputDeviceViewState{
      .rows = std::move(rows),
      .outputBackendSummary = "PW",
      .outputDeviceStatus = "PipeWire: Device 0",
      .hasActiveOutputDevice = true,
    };

    auto regions = std::vector<OutputDeviceRowHitRegion>{};
    auto const rendered = renderElement(englishOutputDevicePanel(view, 1, &regions, 48), 48, 24);

    CHECK_FALSE(rendered.text.contains("very_long_identifier"));
    CHECK_FALSE(rendered.text.contains("toggle"));
    REQUIRE_FALSE(regions.empty());
    auto const selectedRow = regions.front().box;
    CHECK(selectedRow.x_min == 2);
    CHECK(selectedRow.x_max == 45);
    CHECK(rendered.screen.PixelAt(1, selectedRow.y_min).background_color == ftxui::Color::Default);
    CHECK(rendered.screen.PixelAt(2, selectedRow.y_min).background_color == ftxui::Color::Yellow);
    CHECK(rendered.screen.PixelAt(45, selectedRow.y_min).background_color == ftxui::Color::Yellow);
    CHECK(rendered.screen.PixelAt(46, selectedRow.y_min).background_color == ftxui::Color::Default);
  }
} // namespace ao::tui::test
