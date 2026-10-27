// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "GtkNativeInputTestSupport.h"

#include "GtkApplicationTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <gdk/gdk.h>
#include <glib-object.h>
#include <graphene.h>
#include <gtk/gtk.h>
#include <gtkmm/eventcontroller.h>
#include <gtkmm/widget.h>

// X11/Xlib.h defines macros that clash with glibmm headers, so it and the X11
// GDK backend headers must come after every glibmm, gdkmm, or gtkmm include.
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <gdk/x11/gdkx.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <tuple>
#include <utility>

namespace ao::gtk::test
{
  void requireOwnedGtkDisplay()
  {
    auto const* const marker = std::getenv("AOBUS_OWNED_GTK_DISPLAY");

    if (marker == nullptr || std::string_view{marker} != "1")
    {
      SKIP("native GTK input requires the portal-owned Xvfb display");
    }
  }

  struct GtkNativePointerFixture::State final
  {
    explicit State(Gtk::Widget& targetWidget, std::uint32_t const pointerButton)
      : target{targetWidget}, button{pointerButton}
    {
    }

    // The observer counts raw event delivery; gesture claims deny gestures, so
    // this legacy controller keeps seeing every event of a claimed sequence.
    static gboolean observeEvent([[maybe_unused]] GtkEventController* controller, GdkEvent* event, gpointer data)
    {
      switch (auto& state = *static_cast<State*>(data); ::gdk_event_get_event_type(event))
      {
        case GDK_BUTTON_PRESS: ++state.pressCount; break;
        case GDK_MOTION_NOTIFY: ++state.motionCount; break;
        case GDK_BUTTON_RELEASE: ++state.releaseCount; break;
        default: break;
      }

      return FALSE;
    }

    std::pair<std::int32_t, std::int32_t> rootPoint(double const widgetX, double const widgetY) const
    {
      auto* const native = ::gtk_widget_get_native(GTK_WIDGET(target.gobj()));
      REQUIRE(native != nullptr);
      auto const widgetPoint = graphene_point_t{static_cast<float>(widgetX), static_cast<float>(widgetY)};
      auto nativePoint = graphene_point_t{};
      REQUIRE(::gtk_widget_compute_point(GTK_WIDGET(target.gobj()), GTK_WIDGET(native), &widgetPoint, &nativePoint) !=
              FALSE);
      auto* const surface = ::gtk_native_get_surface(native);
      REQUIRE(surface != nullptr);
      REQUIRE(GDK_IS_X11_SURFACE(surface));
      // GTK deprecates X11 access, but native Xvfb input requires its public ID.
      G_GNUC_BEGIN_IGNORE_DEPRECATIONS
      auto const surfaceId = ::gdk_x11_surface_get_xid(surface);
      G_GNUC_END_IGNORE_DEPRECATIONS
      double surfaceX = 0.0;
      double surfaceY = 0.0;
      ::gtk_native_get_surface_transform(native, &surfaceX, &surfaceY);

      int rootX = 0;
      int rootY = 0;
      ::Window child = None;
      REQUIRE(::XTranslateCoordinates(display,
                                      surfaceId,
                                      DefaultRootWindow(display),
                                      static_cast<std::int32_t>(std::lround(nativePoint.x - surfaceX)),
                                      static_cast<std::int32_t>(std::lround(nativePoint.y - surfaceY)),
                                      &rootX,
                                      &rootY,
                                      &child) != False);
      return {rootX, rootY};
    }

    Gtk::Widget& target;
    std::uint32_t button;
    ::Display* display = nullptr;
    ::GtkEventController* observer = nullptr;
    std::int32_t pressCount = 0;
    std::int32_t motionCount = 0;
    std::int32_t releaseCount = 0;
    bool buttonDown = false;
  };

  GtkNativePointerFixture::GtkNativePointerFixture(Gtk::Widget& target, std::uint32_t const button)
    : _statePtr{std::make_unique<State>(target, button)}
  {
    requireOwnedGtkDisplay();
    auto& state = *_statePtr;

    // The target must be mapped: every injected event is routed through the
    // target's realized native surface.
    REQUIRE(state.target.get_mapped());

    auto* const gdkDisplay = ::gtk_widget_get_display(GTK_WIDGET(state.target.gobj()));
    REQUIRE(GDK_IS_X11_DISPLAY(gdkDisplay));
    // This fixture deliberately targets the portal's isolated Xvfb backend.
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    state.display = ::gdk_x11_display_get_xdisplay(gdkDisplay);
    G_GNUC_END_IGNORE_DEPRECATIONS
    REQUIRE(state.display != nullptr);

    int eventBase = 0;
    int errorBase = 0;
    int majorVersion = 0;
    int minorVersion = 0;
    REQUIRE(::XTestQueryExtension(state.display, &eventBase, &errorBase, &majorVersion, &minorVersion) != False);

    state.observer = ::gtk_event_controller_legacy_new();
    ::gtk_event_controller_set_propagation_phase(state.observer, GTK_PHASE_CAPTURE);
    ::g_signal_connect_data(
      state.observer, "event", G_CALLBACK(&State::observeEvent), &state, nullptr, G_CONNECT_DEFAULT);
    ::gtk_widget_add_controller(GTK_WIDGET(state.target.gobj()), state.observer);
  }

  GtkNativePointerFixture::~GtkNativePointerFixture()
  {
    auto& state = *_statePtr;

    if (state.buttonDown && state.display != nullptr)
    {
      std::ignore = ::XTestFakeButtonEvent(state.display, state.button, False, CurrentTime);
      std::ignore = ::XSync(state.display, False);
    }

    if (state.observer != nullptr)
    {
      ::g_signal_handlers_disconnect_matched(state.observer, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, &state);
      ::gtk_widget_remove_controller(GTK_WIDGET(state.target.gobj()), state.observer);
    }
  }

  void GtkNativePointerFixture::movePointerTo(double const widgetX, double const widgetY)
  {
    auto& state = *_statePtr;
    auto const [rootX, rootY] = state.rootPoint(widgetX, widgetY);
    REQUIRE(::XTestFakeMotionEvent(state.display, DefaultScreen(state.display), rootX, rootY, CurrentTime) != False);
    std::ignore = ::XSync(state.display, False);
  }

  void GtkNativePointerFixture::press()
  {
    auto& state = *_statePtr;
    REQUIRE_FALSE(state.buttonDown);
    auto const deliveredBefore = state.pressCount;
    REQUIRE(::XTestFakeButtonEvent(state.display, state.button, True, CurrentTime) != False);
    std::ignore = ::XSync(state.display, False);
    state.buttonDown = true;
    REQUIRE(tryPumpGtkEventsUntil([&] { return state.pressCount > deliveredBefore; }));
  }

  void GtkNativePointerFixture::release(bool const expectDelivery)
  {
    auto& state = *_statePtr;
    REQUIRE(state.buttonDown);
    auto const deliveredBefore = state.releaseCount;
    REQUIRE(::XTestFakeButtonEvent(state.display, state.button, False, CurrentTime) != False);
    std::ignore = ::XSync(state.display, False);
    state.buttonDown = false;

    if (expectDelivery)
    {
      REQUIRE(tryPumpGtkEventsUntil([&] { return state.releaseCount > deliveredBefore; }));
    }
    else
    {
      drainGtkEventsFor(std::chrono::milliseconds{20});
    }
  }

  bool GtkNativePointerFixture::isButtonDown() const noexcept
  {
    return _statePtr->buttonDown;
  }

  std::int32_t GtkNativePointerFixture::pressCount() const noexcept
  {
    return _statePtr->pressCount;
  }

  std::int32_t GtkNativePointerFixture::motionCount() const noexcept
  {
    return _statePtr->motionCount;
  }

  std::int32_t GtkNativePointerFixture::releaseCount() const noexcept
  {
    return _statePtr->releaseCount;
  }
} // namespace ao::gtk::test
