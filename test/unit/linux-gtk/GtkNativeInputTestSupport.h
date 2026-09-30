// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <memory>

namespace Gtk
{
  class Widget;
} // namespace Gtk

namespace ao::gtk::test
{
  /// Skips the enclosing test case unless the portal owns this process's
  /// isolated Xvfb display (AOBUS_OWNED_GTK_DISPLAY=1). Native pointer
  /// injection must never target an inherited desktop display.
  void requireOwnedGtkDisplay();

  /// Real X11/XTest pointer input for one widget subtree.
  ///
  /// The borrowed target must outlive this fixture and stay mapped inside a
  /// presented native window for coordinate-targeted input. A balancing release
  /// after cancellation need not reach the target. Construction checks that
  /// the portal owns the display before admitting native input. Pointer events
  /// use target-relative coordinates through public widget and surface
  /// transforms, and a capture-phase observer counts the press, motion, and
  /// release events really delivered to the target. Expected delivery uses
  /// bounded condition waits; cancellation uses a short timed drain instead.
  /// The destructor releases a still-held button and detaches the observer.
  class GtkNativePointerFixture final
  {
  public:
    explicit GtkNativePointerFixture(Gtk::Widget& target, std::uint32_t button = 1U);
    ~GtkNativePointerFixture();

    GtkNativePointerFixture(GtkNativePointerFixture const&) = delete;
    GtkNativePointerFixture& operator=(GtkNativePointerFixture const&) = delete;
    GtkNativePointerFixture(GtkNativePointerFixture&&) = delete;
    GtkNativePointerFixture& operator=(GtkNativePointerFixture&&) = delete;

    /// Moves the X pointer above the target-relative point.
    void movePointerTo(double widgetX, double widgetY);

    /// Presses the configured button at the current pointer position and waits
    /// bounded for the press to be delivered to the target.
    void press();

    /// Releases the pressed button. With @p expectDelivery the release waits
    /// bounded for delivery; without it a short bounded drain runs instead,
    /// for an interaction whose gesture already ended before the release.
    void release(bool expectDelivery = true);

    bool isButtonDown() const noexcept;
    std::int32_t pressCount() const noexcept;
    std::int32_t motionCount() const noexcept;
    std::int32_t releaseCount() const noexcept;

  private:
    struct State;
    std::unique_ptr<State> _statePtr;
  };
} // namespace ao::gtk::test
