// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "GtkApplicationTestSupport.h"

#include <catch2/catch_session.hpp>
#include <glib.h>

#include <print>
#include <string_view>

namespace
{
  void setDefaultEnv(char const* name, char const* value)
  {
    if (::g_getenv(name) == nullptr)
    {
      ::g_setenv(name, value, TRUE);
    }
  }
} // namespace

int main(int argc, char* argv[])
{
  // Self-reentry probes exercise this same startup path without opening GTK or
  // contacting any bus. All endpoints below are inert fixture strings.
  auto const probe =
    argc == 3 && std::string_view{argv[1]} == "--aobus-probe-child" ? std::string_view{argv[2]} : std::string_view{};

  if (!probe.empty())
  {
    ::g_setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/aobus-inert-probe", TRUE);
    ::g_unsetenv("AOBUS_OWNED_GTK_BUS");
    ::g_log_set_always_fatal(static_cast<::GLogLevelFlags>(G_LOG_FATAL_MASK));

    if (probe == "owned-bus")
    {
      ::g_setenv("AOBUS_OWNED_GTK_BUS", "unix:path=/aobus-inert-probe", TRUE);
    }
    else if (probe == "mismatched-bus")
    {
      ::g_setenv("AOBUS_OWNED_GTK_BUS", "unix:path=/other-inert-probe", TRUE);
    }
    else if (probe != "unowned-bus" && probe != "fatal-warning")
    {
      return 2;
    }
  }

  // Indirect consumers such as MainWindow can start MPRIS too. Never let a
  // direct/IDE/CTest launch discover the user's bus through the runtime directory.
  if (!ao::gtk::test::isOwnedGtkSessionBus(::g_getenv("DBUS_SESSION_BUS_ADDRESS"), ::g_getenv("AOBUS_OWNED_GTK_BUS")))
  {
    ::g_setenv("DBUS_SESSION_BUS_ADDRESS", "disabled:", TRUE);
    ::g_unsetenv("AOBUS_OWNED_GTK_BUS");
  }

  auto const requiredFatalMask =
    static_cast<::GLogLevelFlags>(G_LOG_FATAL_MASK | G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL);
  auto const previousFatalMask = ::g_log_set_always_fatal(requiredFatalMask);
  ::g_log_set_always_fatal(static_cast<::GLogLevelFlags>(previousFatalMask | requiredFatalMask));

  if (!probe.empty())
  {
    if (probe == "fatal-warning")
    {
      ::g_warning("GTK direct-entry warning probe");
      return 0; // A surviving warning must fail the parent regression.
    }

    auto const* const expected = probe == "owned-bus" ? "unix:path=/aobus-inert-probe" : "disabled:";

    if (std::string_view{::g_getenv("DBUS_SESSION_BUS_ADDRESS")} != expected ||
        (probe != "owned-bus" && ::g_getenv("AOBUS_OWNED_GTK_BUS") != nullptr))
    {
      return 1;
    }

    std::println("{}: isolated=yes", probe);
    return 0;
  }

  // These fallback defaults cover direct binary launches. The ao runner and
  // CTest set them before process startup because GTK can select some backends
  // before this main() begins:
  // - GTK_A11Y=test selects the in-process accessibility backend that
  //   gtk_test_accessible_check_property() (see GtkWidgetTestSupport.h)
  //   relies on.
  // - GDK_BACKEND=x11 matches the Xvfb display provided by the test runner,
  //   avoiding backend probing order issues.
  // - GDK_DISABLE=gl,vulkan together with GSK_RENDERER=cairo force the
  //   deterministic Cairo renderer, since GL/Vulkan are unavailable or slow
  //   under Xvfb.
  // An explicitly set value still wins, so debugging with a real display,
  // renderer, or accessibility backend stays possible.
  setDefaultEnv("GTK_A11Y", "test");
  setDefaultEnv("GTK_IM_MODULE", "simple");
  setDefaultEnv("GDK_BACKEND", "x11");
  setDefaultEnv("GDK_DISABLE", "gl,vulkan");
  setDefaultEnv("GSK_RENDERER", "cairo");

  return Catch::Session{}.run(argc, argv);
}
