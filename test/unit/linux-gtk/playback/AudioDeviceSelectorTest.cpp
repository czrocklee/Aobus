// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "playback/AudioDeviceSelector.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"
#include <ao/audio/Backend.h>
#include <ao/audio/IBackend.h>
#include <ao/audio/IBackendProvider.h>
#include <ao/audio/NullBackend.h>
#include <ao/audio/Subscription.h>
#include <ao/rt/PlaybackService.h>

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/enums.h>
#include <gtkmm/listbox.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>

#include <memory>
#include <string_view>
#include <utility>

namespace ao::gtk::test
{
  namespace
  {
    struct StubBackend final : audio::NullBackend
    {
      audio::BackendId backendIdValue;
      audio::ProfileId profileIdValue;

      StubBackend(audio::BackendId backendId, audio::ProfileId profileId)
        : backendIdValue{std::move(backendId)}, profileIdValue{std::move(profileId)}
      {
      }

      audio::BackendId backendId() const noexcept override { return backendIdValue; }
      audio::ProfileId profileId() const noexcept override { return profileIdValue; }
    };

    struct FakeOutputProvider final : audio::IBackendProvider
    {
      Status fakeStatus;

      explicit FakeOutputProvider(Status status)
        : fakeStatus{std::move(status)}
      {
      }

      void shutdown() noexcept override {}

      audio::Subscription subscribeDevices(OnDevicesChangedCallback callback) override
      {
        if (callback)
        {
          callback(fakeStatus.devices);
        }

        return audio::Subscription{};
      }

      Status status() const override { return fakeStatus; }

      std::unique_ptr<audio::IBackend> createBackend(audio::Device const& device,
                                                     audio::ProfileId const& profile) override
      {
        return std::make_unique<StubBackend>(device.backendId, profile);
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

  TEST_CASE("AudioDeviceSelector - lifecycle", "[gtk][playback][output]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto fixture = GtkRuntimeFixture{};
    auto& playback = fixture.runtime().playback();

    SECTION("constructor wires up the popover with a scrolled list box")
    {
      auto selector = AudioDeviceSelector{playback, Gtk::PositionType::BOTTOM};
      drainGtkEvents();

      CHECK(selector.get_autohide());
      CHECK(selector.get_position() == Gtk::PositionType::BOTTOM);

      auto* const scrolled = dynamic_cast<Gtk::ScrolledWindow*>(selector.get_child());
      REQUIRE(scrolled != nullptr);

      auto* const viewport = scrolled->get_child();
      REQUIRE(viewport != nullptr);
      auto* const listBox = dynamic_cast<Gtk::ListBox*>(viewport->get_first_child());
      REQUIRE(listBox != nullptr);
      CHECK(listBox->get_selection_mode() == Gtk::SelectionMode::NONE);
      CHECK(hasCssClass(*listBox, "ao-rich-list"));
    }

    SECTION("refresh populates the list with a backend header and a device row")
    {
      playback.addProvider(std::make_unique<FakeOutputProvider>(buildFakeStatus()));
      drainGtkEvents();

      auto parent = Gtk::Window{};
      auto selector = AudioDeviceSelector{playback, Gtk::PositionType::BOTTOM};
      selector.set_parent(parent);
      emitShow(selector);
      drainGtkEvents();

      auto* const scrolled = dynamic_cast<Gtk::ScrolledWindow*>(selector.get_child());
      REQUIRE(scrolled != nullptr);
      auto* const viewport = scrolled->get_child();
      REQUIRE(viewport != nullptr);
      auto* const listBox = dynamic_cast<Gtk::ListBox*>(viewport->get_first_child());
      REQUIRE(listBox != nullptr);

      auto rowCount = 0;

      for (auto* child = listBox->get_first_child(); child != nullptr; child = child->get_next_sibling())
      {
        ++rowCount;
      }

      CHECK(rowCount >= 2);
    }
  }
} // namespace ao::gtk::test
