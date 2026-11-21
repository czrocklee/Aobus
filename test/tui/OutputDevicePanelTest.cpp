// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tui/OutputDevicePanel.h"

#include "test/MessageCatalogTestSupport.h"
#include "test/tui/KeymapTestSupport.h"
#include "test/tui/RenderTestSupport.h"
#include "tui/AnchoredOverlay.h"
#include "tui/HitRegions.h"
#include <ao/audio/BackendIds.h>
#include <ao/audio/Device.h>
#include <ao/audio/OutputDeviceSelection.h>
#include <ao/i18n/MessageCatalog.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <cstdint>
#include <format>
#include <string>
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

  TEST_CASE("OutputDevicePanel - renders grouped selectable rows")
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

  TEST_CASE("OutputDevicePanel - a waiting output has one advisory and only alternatives can be selected")
  {
    auto const view = uimodel::OutputDeviceViewState{
      .rows = {{.kind = uimodel::OutputDeviceRow::Kind::Status, .title = "Requested output unavailable"},
               {.kind = uimodel::OutputDeviceRow::Kind::DeviceProfile,
                .backendId = audio::kBackendPipeWire,
                .deviceId = audio::DeviceId{"speakers"},
                .profileId = audio::kProfileShared,
                .title = "Speakers"}},
      .outputBackendSummary = "Choose output",
      .outputDeviceStatus = "Requested output unavailable",
    };

    for (auto const width : {80, 120})
    {
      auto hits = std::vector<OutputDeviceRowHitRegion>{};
      auto const text =
        renderText(englishOutputDevicePanel(view, 1, &hits, englishOutputDevicePanelColumns(view, width)), width);
      CHECK(text.contains(view.outputBackendSummary));
      REQUIRE(text.contains(view.outputDeviceStatus));
      CHECK(text.find(view.outputDeviceStatus) == text.rfind(view.outputDeviceStatus));
      CHECK(text.contains("Speakers"));
      REQUIRE(hits.size() == 1);
      CHECK(hits.front().rowIndex == 1);
      CHECK(hits.front().deviceId == audio::DeviceId{"speakers"});
    }
  }

  TEST_CASE("OutputDevicePanel - narrow picker shows fixed exclusive help and the pending-output advisory")
  {
    using RowKind = uimodel::OutputDeviceRow::Kind;
    using i18n::MessageId;

    auto const& catalog = ao::test::englishMessageCatalog();
    auto const status =
      i18n::requiredFormat(catalog, MessageId::OutputDevicePendingNamedStatus, {{"device", "Travel DAC"}});
    auto const expected = audio::OutputDeviceSelection{.backendId = audio::kBackendPipeWire,
                                                       .deviceId = audio::DeviceId{"node:missing"},
                                                       .profileId = audio::kProfileExclusive};
    auto const view = uimodel::OutputDeviceViewState{
      .rows = {{.kind = RowKind::Status, .title = status},
               {.kind = RowKind::RoutingPolicy,
                .title = "Let system use other outputs",
                .description = i18n::requiredFormat(
                  catalog, MessageId::OutputDeviceRoutingPolicyExclusive, {{"rerouting", "fixed"}}),
                .optRoutingPolicySelection = expected},
               {.kind = RowKind::DeviceProfile,
                .backendId = audio::kBackendPipeWire,
                .profileId = audio::kProfileShared,
                .title = "System Default"}},
      .outputBackendSummary = "Choose output",
      .outputDeviceStatus = status,
    };
    auto hits = std::vector<OutputDeviceRowHitRegion>{};
    auto const text = renderText(englishOutputDevicePanel(view, 1, &hits, 40), 40);

    CHECK(text.contains("[ ] Let system use other outputs"));
    CHECK(text.contains("Off: use only this device."));
    CHECK(text.contains("Exclusive Mode"));
    CHECK(text.contains("busy."));
    CHECK(text.contains("other apps"));
    CHECK(text.contains("Requested output unavailable:"));
    CHECK(text.contains("Travel DAC"));
    CHECK(text.find("Requested output") == text.rfind("Requested output"));
    CHECK(text.contains("System Default"));
    CHECK(text.contains("Enter select"));
    REQUIRE(hits.size() == 2);
    CHECK(hits[0].kind == RowKind::RoutingPolicy);
    CHECK(hits[0].optRoutingPolicySelection == expected);
    CHECK(hasHitArea(hits[0].box));
    CHECK_FALSE(hasHitArea(hits[0].secondaryBox));
    CHECK(hits[1].kind == RowKind::DeviceProfile);
    CHECK_FALSE(hits[1].optRoutingPolicySelection);
  }

  TEST_CASE("OutputDevicePanel - anchored narrow picker preserves French exclusive routing help")
  {
    using RowKind = uimodel::OutputDeviceRow::Kind;
    using i18n::MessageId;

    auto const french = ao::test::messageCatalog("fr");
    auto view = uimodel::OutputDeviceViewState{
      .rows = {{.kind = RowKind::RoutingPolicy,
                .title = std::string{i18n::requiredText(french, MessageId::OutputDeviceRoutingPolicyToggle)},
                .description = i18n::requiredFormat(
                  french, MessageId::OutputDeviceRoutingPolicyExclusive, {{"rerouting", "allowed"}}),
                .isActive = true,
                .optRoutingPolicySelection =
                  audio::OutputDeviceSelection{.backendId = audio::kBackendPipeWire,
                                               .deviceId = audio::DeviceId{"node:missing"},
                                               .profileId = audio::kProfileExclusive,
                                               .routingPolicy = audio::OutputRoutingPolicy::AllowSystemReroute}}},
      .outputBackendSummary = std::string{i18n::requiredText(french, MessageId::OutputDevicePendingSummary)},
      .outputDeviceStatus = std::string{i18n::requiredText(french, MessageId::OutputDevicePendingStatus)},
    };

    // Overflow the viewport so the scrollbar is painted beside the help text.
    for (std::int32_t index = 0; index < 20; ++index)
    {
      auto const name = std::format("DAC {}", index);
      view.rows.push_back({.kind = RowKind::DeviceProfile,
                           .backendId = audio::kBackendPipeWire,
                           .deviceId = audio::DeviceId{name},
                           .profileId = audio::kProfileShared,
                           .title = name});
    }

    constexpr std::int32_t kTerminalColumns = 48;
    constexpr std::int32_t kTerminalRows = 24;
    auto const& keymap = defaultKeymapPlan();
    auto const panelColumns = outputDevicePanelColumns(french, view, keymap, kTerminalColumns);
    auto const overlayPtr = anchoredOverlay(outputDevicePanel(french, view, 0, keymap, nullptr, panelColumns),
                                            {.x_min = 20, .x_max = 23, .y_min = 0, .y_max = 0},
                                            AnchoredOverlayPlacement::Below,
                                            {.columns = panelColumns},
                                            {.columns = kTerminalColumns, .rows = kTerminalRows});
    auto const rendered = renderElement(overlayPtr, kTerminalColumns, kTerminalRows);

    CHECK(rendered.text.contains("Périphériques de sortie"));
    CHECK(rendered.text.contains("[x] Autoriser une autre sortie"));
    CHECK(rendered.text.contains("exclusif,"));
    CHECK(rendered.text.contains("déconnecte."));
    CHECK(rendered.text.contains("occupée."));
    CHECK(rendered.text.contains("accès."));
    CHECK(rendered.text.contains("Sortie demandée indisponible"));
    CHECK(rendered.text.contains("Esc fermer"));
  }

  TEST_CASE("OutputDevicePanel - empty state uses the selected locale")
  {
    auto const german = ao::test::messageCatalog("de-AT");
    auto const view = uimodel::OutputDeviceViewState{};
    auto const text = renderText(outputDevicePanel(german, view, 0, defaultKeymapPlan()), 96);

    CHECK(text.contains("Ausgabegeräte"));
    CHECK(text.contains("Keine Ausgabegeräte gefunden"));
    CHECK(text.contains("Kein Ausgabegerät ausgewählt"));
  }

  TEST_CASE("OutputDevicePanel - status-only input keeps the empty list wide enough")
  {
    using i18n::MessageId;

    // With the Spanish pending copy, the empty message needs more room than the header or footer.
    auto const catalog = ao::test::messageCatalog("es");
    auto view = uimodel::OutputDeviceViewState{
      .outputBackendSummary = std::string{i18n::requiredText(catalog, MessageId::OutputDevicePendingSummary)},
      .outputDeviceStatus = std::string{i18n::requiredText(catalog, MessageId::OutputDevicePendingStatus)},
    };
    auto const& keymap = defaultKeymapPlan();
    constexpr std::int32_t kTerminalColumns = 96;
    auto const emptyColumns = outputDevicePanelColumns(catalog, view, keymap, kTerminalColumns);
    view.rows.push_back({.kind = uimodel::OutputDeviceRow::Kind::Status, .title = view.outputDeviceStatus});

    auto const columns = outputDevicePanelColumns(catalog, view, keymap, kTerminalColumns);
    auto hits = std::vector<OutputDeviceRowHitRegion>{};
    auto const text = renderText(outputDevicePanel(catalog, view, -1, keymap, &hits, columns), columns);

    CHECK(columns == emptyColumns);
    CHECK(text.contains(i18n::requiredText(catalog, MessageId::TuiPlaybackNoOutputDevicesFound)));
    CHECK(hits.empty());
    REQUIRE(text.contains(view.outputDeviceStatus));
    CHECK(text.find(view.outputDeviceStatus) == text.rfind(view.outputDeviceStatus));
  }

  TEST_CASE("OutputDevicePanel - width follows content and terminal bounds")
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
    CHECK(englishOutputDevicePanelColumns(view, 40) == 38);
  }

  TEST_CASE("OutputDevicePanel - frames long device lists")
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
