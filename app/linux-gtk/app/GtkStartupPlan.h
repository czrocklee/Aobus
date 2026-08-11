// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/rt/Log.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::gtk
{
  inline constexpr std::string_view kSuccessorOption = "--aobus-successor";
  inline constexpr std::string_view kLibraryRootOption = "--library-root";
  inline constexpr std::string_view kScanAfterOpenOption = "--scan-after-open";

  enum class GtkApplicationRegistrationMode : std::uint8_t
  {
    AllowReplacement,
    ReplaceExisting,
  };

  struct GtkStartupPlan final
  {
    GtkApplicationRegistrationMode registrationMode = GtkApplicationRegistrationMode::AllowReplacement;
    std::optional<std::filesystem::path> optSuccessorLibraryRoot{};
    rt::LogLevel logLevel = rt::LogLevel::Info;
    std::int32_t exitCode = 0;
    bool scanAfterOpen = false;
    bool shouldExit = false;
    bool showVersion = false;
    std::vector<std::string> gtkArguments{};

    friend bool operator==(GtkStartupPlan const&, GtkStartupPlan const&) = default;
  };

  /**
   * @brief Partitions a complete process argv before Gtk::Application is created.
   *
   * The input includes argv[0]. One CLI11 parser consumes Aobus options and the
   * paired private successor protocol, then returns every unclaimed argument to
   * GTK in its original order.
   */
  Result<GtkStartupPlan> planGtkStartup(std::span<std::string_view const> arguments);

  /** Returns a native-diagnostic message when a successor exits before activation. */
  std::optional<std::string> incompleteSuccessorStartupDiagnostic(GtkApplicationRegistrationMode registrationMode,
                                                                  bool startupCompleted,
                                                                  std::int32_t exitCode);
} // namespace ao::gtk
