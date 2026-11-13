// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "GtkSessionBusTestSupport.h"
#include "test/fatal/ProbeProcess.h"

#include <catch2/catch_session.hpp>
#include <glib.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <print>
#include <span>
#include <string_view>

namespace
{
  constexpr char const* kFrameSubmissionWarning = "gdk_frame_timings_submitted() called on submitted frame.";
  constexpr auto kFrameWarningProbes = std::to_array<std::string_view>({
    "frame-warning",
    "frame-warning-sized",
    "frame-warning-other-message",
    "frame-warning-other-domain",
    "frame-warning-critical",
    "frame-warning-error",
    "frame-warning-explicit-fatal",
    "frame-warning-trailing-byte",
    "frame-warning-legacy",
    "frame-warning-unowned-display",
    "frame-warning-unowned-bus",
    "frame-warning-no-display",
    "frame-warning-wayland",
    "frame-warning-renderer",
    "frame-warning-duplicate-domain",
    "frame-warning-missing-domain",
  });

  bool matchesEnv(char const* name, std::string_view expected) noexcept
  {
    auto const* const value = ::g_getenv(name);
    return value != nullptr && value == expected;
  }

  bool hasExactLogField(std::span<::GLogField const> fields, std::string_view key, std::string_view expected) noexcept
  {
    bool found = false;

    for (auto const& field : fields)
    {
      if (field.key == nullptr || std::string_view{field.key} != key)
      {
        continue;
      }

      if (found || field.value == nullptr)
      {
        return false;
      }

      auto const* const bytes = static_cast<char const*>(field.value);
      auto const value =
        field.length < 0 ? std::string_view{bytes} : std::string_view{bytes, static_cast<std::size_t>(field.length)};

      if (value != expected)
      {
        return false;
      }

      found = true;
    }

    return found;
  }

  ::GLogWriterOutput writeGtkTestLog(::GLogLevelFlags level,
                                     ::GLogField const* fields,
                                     ::gsize count,
                                     void* /*userData*/) noexcept
  {
    auto const record = std::span{fields, count};

    auto const legacyRecord =
      std::ranges::any_of(record,
                          [](::GLogField const& field)
                          { return field.key != nullptr && std::string_view{field.key} == "GLIB_OLD_LOG_API"; });

    if (level == G_LOG_LEVEL_WARNING && !legacyRecord && hasExactLogField(record, "GLIB_DOMAIN", "Gdk") &&
        hasExactLogField(record, "MESSAGE", kFrameSubmissionWarning))
    {
      // Keep the original warning visible without the default writer's fatal
      // promotion. Never recurse into GLib logging from this callback.
      if (std::fputs("Aobus GTK test: temporary GTK 4.24.0 X11/Cairo frame-submission exception\n", stderr) >= 0 &&
          ::g_log_writer_standard_streams(level, fields, count, nullptr) == G_LOG_WRITER_HANDLED)
      {
        return G_LOG_WRITER_HANDLED;
      }
    }

    return ::g_log_writer_default(level, fields, count, nullptr);
  }

  void installGtkTestLogWriter()
  {
    // Temporary upstream exception, not an Aobus lifecycle fix: X11 surfaces
    // sharing a frame clock can resubmit before after-paint fills throttling
    // metadata (GTK !10035). Retire after an SDK fix passes popup/no-popup and
    // GTK/ASan controls. No production process installs this writer.
    if (auto const* const display = ::g_getenv("DISPLAY");
        ::gtk_get_major_version() == 4 && ::gtk_get_minor_version() == 24 && ::gtk_get_micro_version() == 0 &&
        matchesEnv("AOBUS_OWNED_GTK_DISPLAY", "1") && display != nullptr &&
        std::string_view{display}.starts_with(":") && matchesEnv("GDK_BACKEND", "x11") &&
        matchesEnv("GSK_RENDERER", "cairo") &&
        ao::gtk::test::isOwnedGtkSessionBus(::g_getenv("DBUS_SESSION_BUS_ADDRESS"), ::g_getenv("AOBUS_OWNED_GTK_BUS")))
    {
      // Once per process, before test dispatch; no borrowed callback state.
      ::g_log_set_writer_func(writeGtkTestLog, nullptr, nullptr);
    }
  }

  std::int32_t runFrameWarningProbe(std::string_view probe)
  {
    installGtkTestLogWriter();
    auto level = G_LOG_LEVEL_WARNING;
    auto fields = std::to_array<::GLogField>({
      {"GLIB_DOMAIN", "Gdk", -1},
      {"MESSAGE", kFrameSubmissionWarning, -1},
      {"CODE_FILE", "inert-frame-warning-probe", -1},
    });

    if (probe == "frame-warning-sized")
    {
      fields[0] = {"GLIB_DOMAIN", "GdkX", 3};
      fields[1] = {"MESSAGE",
                   "gdk_frame_timings_submitted() called on submitted frame.X",
                   static_cast<::gssize>(std::string_view{kFrameSubmissionWarning}.size())};
    }
    else if (probe == "frame-warning-other-message")
    {
      fields[1].value = "gdk_frame_timings_submitted() called on submitted frame.X";
    }
    else if (probe == "frame-warning-other-domain")
    {
      fields[0].value = "GdkX";
    }
    else if (probe == "frame-warning-critical")
    {
      level = G_LOG_LEVEL_CRITICAL;
    }
    else if (probe == "frame-warning-error")
    {
      level = G_LOG_LEVEL_ERROR;
    }
    else if (probe == "frame-warning-explicit-fatal")
    {
      level = static_cast<::GLogLevelFlags>(G_LOG_LEVEL_WARNING | G_LOG_FLAG_FATAL);
    }
    else if (probe == "frame-warning-trailing-byte")
    {
      fields[1].length = static_cast<::gssize>(std::string_view{kFrameSubmissionWarning}.size() + 1);
    }
    else if (probe == "frame-warning-duplicate-domain")
    {
      fields[2] = {"GLIB_DOMAIN", "Gdk", -1};
    }
    else if (probe == "frame-warning-missing-domain")
    {
      fields[0].key = "OTHER_DOMAIN";
    }

    std::fputs("GTK frame-warning probe: emitting\n", stderr);

    if (probe == "frame-warning-legacy")
    {
      ::g_log("Gdk", level, "%s", kFrameSubmissionWarning);
    }
    else
    {
      ::g_log_structured_array(level, fields.data(), fields.size());
    }

    std::println("{}: survived=yes", probe);
    return 0;
  }

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
    ao::test::reportProbeChildStarted();
    ao::test::suppressProbeChildCrashDump();
    ::g_setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/aobus-inert-probe", TRUE);
    ::g_unsetenv("AOBUS_OWNED_GTK_BUS");
    ::g_log_set_always_fatal(static_cast<::GLogLevelFlags>(G_LOG_FATAL_MASK));

    if (std::ranges::contains(kFrameWarningProbes, probe))
    {
      ::g_setenv("AOBUS_OWNED_GTK_BUS", "unix:path=/aobus-inert-probe", TRUE);
      ::g_setenv("AOBUS_OWNED_GTK_DISPLAY", "1", TRUE);
      ::g_setenv("DISPLAY", ":aobus-inert-probe", TRUE);
      ::g_setenv("GDK_BACKEND", "x11", TRUE);
      ::g_setenv("GSK_RENDERER", "cairo", TRUE);

      if (probe == "frame-warning-unowned-display")
      {
        ::g_unsetenv("AOBUS_OWNED_GTK_DISPLAY");
      }
      else if (probe == "frame-warning-unowned-bus")
      {
        ::g_unsetenv("AOBUS_OWNED_GTK_BUS");
      }
      else if (probe == "frame-warning-no-display")
      {
        ::g_unsetenv("DISPLAY");
      }
      else if (probe == "frame-warning-wayland")
      {
        ::g_setenv("GDK_BACKEND", "wayland", TRUE);
      }
      else if (probe == "frame-warning-renderer")
      {
        ::g_setenv("GSK_RENDERER", "gl", TRUE);
      }
    }
    else if (probe == "owned-bus")
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
    if (std::ranges::contains(kFrameWarningProbes, probe))
    {
      return runFrameWarningProbe(probe);
    }

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
  installGtkTestLogWriter();

  return Catch::Session{}.run(argc, argv);
}
