// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/RuntimeTestUtils.h"
#include <ao/audio/Backend.h>
#include <ao/audio/IBackend.h>
#include <ao/audio/IBackendProvider.h>
#include <ao/audio/NullBackend.h>
#include <ao/audio/Subscription.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/ViewService.h>
#include <ao/rt/library/LibraryChanges.h>
#include <ao/rt/library/LibraryWriter.h>
#include <ao/rt/source/ListSourceStore.h>
#include <ao/uimodel/playback/output/OutputDeviceViewModel.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::uimodel::test
{
  using namespace ao::rt::test;
  using namespace ao::rt;
  namespace
  {
    // ── FakeOutputDeviceProvider: hand-written fake IBackendProvider ──────────────
    // Unlike mocking frameworks, this directly controls the callback chain:
    // subscribeDevices immediately invokes the callback, which triggers
    // Player → PlaybackService.onOutputDevicesChanged → state.availableOutputBackends rebuild.
    //
    // Uses TestBackend (below) instead of NullBackend so that backendId() and
    // profileId() return the expected values, enabling setOutputDevice + isActive tests.

    struct TestBackend final : audio::NullBackend
    {
      audio::BackendId backendIdValue;
      audio::ProfileId profileIdValue;

      TestBackend(audio::BackendId backendId, audio::ProfileId profileId)
        : backendIdValue{std::move(backendId)}, profileIdValue{std::move(profileId)}
      {
      }

      audio::BackendId backendId() const noexcept override { return backendIdValue; }
      audio::ProfileId profileId() const noexcept override { return profileIdValue; }
    };

    struct FakeOutputDeviceProvider final : audio::IBackendProvider
    {
      Status provStatus;

      explicit FakeOutputDeviceProvider(Status status)
        : provStatus{std::move(status)}
      {
      }

      void shutdown() noexcept override {}

      audio::Subscription subscribeDevices(OnDevicesChangedCallback callback) override
      {
        if (callback)
        {
          callback(provStatus.devices);
        }

        return audio::Subscription{};
      }

      Status status() const override { return provStatus; }

      std::unique_ptr<audio::IBackend> createBackend(audio::Device const& device,
                                                     audio::ProfileId const& profile) override
      {
        return std::make_unique<TestBackend>(device.backendId, profile);
      }

      audio::Subscription subscribeGraph(std::string_view /*routeAnchor*/, OnGraphChangedCallback /*callback*/) override
      {
        return audio::Subscription{};
      }
    };

    audio::IBackendProvider::Status buildFakeStatus()
    {
      return {
        .metadata =
          {
            .id = audio::BackendId{"pipewire"},
            .name = "PipeWire",
            .description = "PipeWire Sound Server",
            .iconName = "pipewire",
            .supportedProfiles =
              {
                {.id = audio::kProfileShared, .name = "Shared", .description = "Shared mode"},
                {.id = audio::kProfileExclusive, .name = "Exclusive", .description = "Exclusive mode"},
              },
          },
        .devices =
          {
            {
              .id = audio::DeviceId{"device1"},
              .displayName = "Built-in Audio",
              .description = "Built-in analog stereo",
              .isDefault = true,
              .backendId = audio::BackendId{"pipewire"},
            },
          },
      };
    }
  } // namespace

  TEST_CASE("OutputDeviceViewModel - state generation", "[uimodel][unit][playback][output]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto changes = LibraryChanges{};
    auto listSourceStore = ListSourceStore{testLib.library(), changes};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};
    auto playback = PlaybackService{executor, viewService, testLib.library()};

    auto log = RenderLog<OutputDeviceViewState>{};
    auto viewModel = OutputDeviceViewModel{playback, [&log](auto const& view) { log.render(view); }};

    SECTION("Initial state is empty when no outputs registered")
    {
      viewModel.refresh();
      REQUIRE(!log.empty());
      CHECK(log.last().rows.empty());
    }

    SECTION("selectOutputDevice without registered outputs keeps the view state empty")
    {
      viewModel.selectOutputDevice(audio::BackendId{"t"}, audio::DeviceId{"d"}, audio::kProfileShared);
      viewModel.refresh();

      REQUIRE(!log.empty());
      CHECK(log.last().rows.empty());
    }
  }

  TEST_CASE("OutputDeviceViewModel - refresh with fake provider", "[uimodel][unit][playback][output]")
  {
    auto testLib = TestMusicLibrary{};
    auto executor = MockExecutor{};
    auto changes = LibraryChanges{};
    auto listSourceStore = ListSourceStore{testLib.library(), changes};
    auto viewService = ViewService{executor, testLib.library(), listSourceStore};
    auto playback = PlaybackService{executor, viewService, testLib.library()};

    auto log = RenderLog<OutputDeviceViewState>{};
    auto viewModel = OutputDeviceViewModel{playback, [&log](auto const& view) { log.render(view); }};

    SECTION("refresh shows backend header and device×profile rows")
    {
      playback.addProvider(std::make_unique<FakeOutputDeviceProvider>(buildFakeStatus()));
      viewModel.refresh();

      REQUIRE(!log.empty());
      auto const& rows = log.last().rows;
      REQUIRE(rows.size() == 3); // 1 header + 1 device × 2 profiles

      // Row 0: BackendHeader
      CHECK(rows[0].kind == OutputDeviceRow::Kind::BackendHeader);
      CHECK(rows[0].backendId == audio::BackendId{"pipewire"});
      CHECK(rows[0].title == "PipeWire");

      // Rows 1-2: DeviceProfile (Shared + Exclusive)
      for (std::size_t i = 1; i < rows.size(); ++i)
      {
        CHECK(rows[i].kind == OutputDeviceRow::Kind::DeviceProfile);
        CHECK(rows[i].deviceId == audio::DeviceId{"device1"});
        CHECK(rows[i].backendId == audio::BackendId{"pipewire"});
      }

      CHECK(rows[1].profileId == audio::kProfileShared);
      CHECK(rows[1].title == "Built-in Audio");
      CHECK(rows[1].description == "Built-in analog stereo");
      CHECK(rows[1].isExclusive == false);
      CHECK(rows[2].profileId == audio::kProfileExclusive);
      CHECK(rows[2].title == "Built-in Audio");
      CHECK(rows[2].description == "Built-in analog stereo");
      CHECK(rows[2].isExclusive == true);
    }

    SECTION("system default output is not offered as an exclusive target")
    {
      auto status = buildFakeStatus();
      status.devices = {
        audio::Device{.id = audio::DeviceId{},
                      .displayName = "System Default",
                      .description = "PipeWire",
                      .isDefault = true,
                      .backendId = audio::BackendId{"pipewire"}},
      };

      playback.addProvider(std::make_unique<FakeOutputDeviceProvider>(std::move(status)));
      viewModel.refresh();

      REQUIRE(!log.empty());
      auto const& rows = log.last().rows;
      REQUIRE(rows.size() == 2);
      CHECK(rows[1].kind == OutputDeviceRow::Kind::DeviceProfile);
      CHECK(rows[1].title == "System Default");
      CHECK(rows[1].profileId == audio::kProfileShared);
      CHECK(rows[1].isExclusive == false);
    }

    SECTION("active device is highlighted after setOutputDevice")
    {
      playback.addProvider(std::make_unique<FakeOutputDeviceProvider>(buildFakeStatus()));
      playback.setOutputDevice(audio::BackendId{"pipewire"}, audio::DeviceId{"device1"}, audio::kProfileExclusive);
      viewModel.refresh();

      auto const& rows = log.last().rows;
      // The Exclusive profile row should be active
      CHECK(rows[2].isActive == true);
      CHECK(rows[2].profileId == audio::kProfileExclusive);
      // The Shared profile row should NOT be active
      CHECK(rows[1].isActive == false);
    }

    SECTION("selectOutputDevice triggers playback state change")
    {
      playback.addProvider(std::make_unique<FakeOutputDeviceProvider>(buildFakeStatus()));
      viewModel.selectOutputDevice(audio::BackendId{"pipewire"}, audio::DeviceId{"device1"}, audio::kProfileShared);

      auto const& sel = playback.state().selectedOutputDevice;
      CHECK(sel.backendId == audio::BackendId{"pipewire"});
      CHECK(sel.deviceId == audio::DeviceId{"device1"});
      CHECK(sel.profileId == audio::kProfileShared);
    }

    SECTION("multiple backends produce separate header rows")
    {
      auto status2 = buildFakeStatus();
      status2.metadata.id = audio::BackendId{"alsa"};
      status2.metadata.name = "ALSA";
      status2.devices[0].backendId = audio::BackendId{"alsa"};
      status2.devices[0].id = audio::DeviceId{"alsa-device1"};

      playback.addProvider(std::make_unique<FakeOutputDeviceProvider>(buildFakeStatus()));
      playback.addProvider(std::make_unique<FakeOutputDeviceProvider>(std::move(status2)));
      viewModel.refresh();

      auto const& rows = log.last().rows;
      // Should have: PipeWire header + 2 PipeWire profiles + ALSA header + 2 ALSA profiles = 6 rows
      REQUIRE(rows.size() == 6);

      CHECK(rows[0].kind == OutputDeviceRow::Kind::BackendHeader);
      CHECK(rows[0].title == "PipeWire");
      CHECK(rows[3].kind == OutputDeviceRow::Kind::BackendHeader);
      CHECK(rows[3].title == "ALSA");
    }

    SECTION("summary fields for PipeWire shared output")
    {
      playback.addProvider(std::make_unique<FakeOutputDeviceProvider>(buildFakeStatus()));
      playback.setOutputDevice(audio::BackendId{"pipewire"}, audio::DeviceId{"device1"}, audio::kProfileShared);
      viewModel.refresh();

      auto const& view = log.last();
      CHECK(view.hasActiveOutputDevice == true);
      CHECK(view.outputBackendSummary == "PW");
      CHECK(view.outputDeviceStatus == "PipeWire: Built-in Audio");
    }

    SECTION("summary fields for ALSA exclusive output")
    {
      auto status = buildFakeStatus();
      status.metadata.id = audio::BackendId{"alsa"};
      status.metadata.name = "ALSA";
      status.devices[0].backendId = audio::BackendId{"alsa"};
      status.devices[0].displayName = "USB DAC";

      playback.addProvider(std::make_unique<FakeOutputDeviceProvider>(std::move(status)));
      playback.setOutputDevice(audio::BackendId{"alsa"}, audio::DeviceId{"device1"}, audio::kProfileExclusive);
      viewModel.refresh();

      auto const& view = log.last();
      CHECK(view.hasActiveOutputDevice == true);
      CHECK(view.outputBackendSummary == "ALSA");
      CHECK(view.outputDeviceStatus == "ALSA: USB DAC (Exclusive Mode)");
    }

    SECTION("summary fields when no output is selected")
    {
      // No provider added, so no output device can be selected
      viewModel.refresh();

      auto const& view = log.last();
      CHECK(view.hasActiveOutputDevice == false);
      CHECK(view.outputBackendSummary == "--");
      CHECK(view.outputDeviceStatus.empty());
    }
  }
} // namespace ao::uimodel::test
