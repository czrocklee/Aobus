// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "common/PopoverAttachment.h"

#include "test/unit/linux-gtk/GtkTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <gtkmm/button.h>
#include <gtkmm/popover.h>
#include <gtkmm/window.h>

#include <cstdint>
#include <memory>
#include <utility>

namespace ao::gtk::test
{
  namespace
  {
    class TrackingPopover final : public Gtk::Popover
    {
    public:
      explicit TrackingPopover(std::int32_t& destructionCount)
        : _destructionCount{destructionCount}
      {
      }

      TrackingPopover(TrackingPopover const&) = delete;
      TrackingPopover& operator=(TrackingPopover const&) = delete;
      TrackingPopover(TrackingPopover&&) = delete;
      TrackingPopover& operator=(TrackingPopover&&) = delete;

      ~TrackingPopover() override { ++_destructionCount; }

    private:
      std::int32_t& _destructionCount;
    };
  } // namespace

  TEST_CASE("PopoverAttachment - close unparents immediately and retires after GTK dispatch",
            "[gtk][unit][common][popover-lifetime]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    auto anchor = Gtk::Button{};
    std::int32_t destructionCount = 0;
    auto attachment = PopoverAttachment{};
    auto popoverPtr = std::make_unique<TrackingPopover>(destructionCount);
    auto* const observedPopover = popoverPtr.get();

    attachment.attach(std::move(popoverPtr), anchor);

    REQUIRE(attachment.hasPopover());
    REQUIRE(observedPopover->get_parent() == &anchor);

    emitClosed(*observedPopover);

    CHECK_FALSE(attachment.hasPopover());
    CHECK(observedPopover->get_parent() == nullptr);
    CHECK(destructionCount == 0);

    drainGtkEvents();

    CHECK(destructionCount == 1);
  }

  TEST_CASE("PopoverAttachment - anchor unmap retires attachment before replacement",
            "[gtk][regression][popover-lifetime]")
  {
    [[maybe_unused]] auto const appPtr = ensureGtkApplication();
    std::int32_t destructionCount = 0;
    auto anchor = Gtk::Button{};
    auto replacementAnchor = Gtk::Button{};
    auto window = Gtk::Window{};
    auto attachment = PopoverAttachment{};
    window.set_child(anchor);
    window.present();
    drainGtkEvents();

    auto firstPopoverPtr = std::make_unique<TrackingPopover>(destructionCount);
    auto* const observedFirstPopover = firstPopoverPtr.get();
    attachment.attach(std::move(firstPopoverPtr), anchor);

    window.unset_child();

    CHECK_FALSE(attachment.hasPopover());
    CHECK(observedFirstPopover->get_parent() == nullptr);
    CHECK(destructionCount == 0);

    auto replacementPopoverPtr = std::make_unique<TrackingPopover>(destructionCount);
    auto* const observedReplacementPopover = replacementPopoverPtr.get();
    window.set_child(replacementAnchor);
    attachment.attach(std::move(replacementPopoverPtr), replacementAnchor);

    REQUIRE(attachment.hasPopover());
    CHECK(observedReplacementPopover->get_parent() == &replacementAnchor);

    drainGtkEvents();

    CHECK(attachment.hasPopover());
    CHECK(destructionCount == 1);

    emitClosed(*observedReplacementPopover);
    drainGtkEvents();

    CHECK_FALSE(attachment.hasPopover());
    CHECK(destructionCount == 2);
  }
} // namespace ao::gtk::test
