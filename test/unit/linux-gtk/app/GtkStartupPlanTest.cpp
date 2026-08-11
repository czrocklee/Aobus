// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "app/GtkStartupPlan.h"

#include <ao/Error.h>
#include <ao/rt/Log.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ao::gtk::test
{
  TEST_CASE("GtkStartupPlan - ordinary startup partitions Aobus options from GTK arguments", "[gtk][unit][app]")
  {
    auto const arguments = std::array<std::string_view, 7>{
      "aobus-gtk", "--display=:7", "-vv", "--log-level", "debug", "music.aobus", "--name=Aobus"};

    auto result = planGtkStartup(arguments);

    REQUIRE(result);
    CHECK(result->registrationMode == GtkApplicationRegistrationMode::AllowReplacement);
    CHECK_FALSE(result->optSuccessorLibraryRoot);
    CHECK_FALSE(result->scanAfterOpen);
    CHECK(result->logLevel == rt::LogLevel::Trace);
    CHECK_FALSE(result->shouldExit);
    CHECK(result->gtkArguments == std::vector<std::string>{"aobus-gtk", "--display=:7", "music.aobus", "--name=Aobus"});
  }

  TEST_CASE("GtkStartupPlan - GLib replacement option remains GTK passthrough", "[gtk][unit][app]")
  {
    auto const arguments = std::array<std::string_view, 3>{"aobus-gtk", "--gapplication-replace", "--display=:7"};

    auto result = planGtkStartup(arguments);

    REQUIRE(result);
    CHECK(result->registrationMode == GtkApplicationRegistrationMode::AllowReplacement);
    CHECK_FALSE(result->optSuccessorLibraryRoot);
    CHECK(result->gtkArguments == std::vector<std::string>{"aobus-gtk", "--gapplication-replace", "--display=:7"});
  }

  TEST_CASE("GtkStartupPlan - Aobus help and version are owned by the single CLI parser", "[gtk][unit][app]")
  {
    SECTION("help")
    {
      auto const arguments = std::array<std::string_view, 2>{"aobus-gtk", "--help"};

      auto result = planGtkStartup(arguments);

      REQUIRE(result);
      CHECK(result->shouldExit);
      CHECK(result->exitCode == 0);
      CHECK_FALSE(result->showVersion);
    }

    SECTION("version")
    {
      auto const arguments = std::array<std::string_view, 2>{"aobus-gtk", "--version"};

      auto result = planGtkStartup(arguments);

      REQUIRE(result);
      CHECK(result->shouldExit);
      CHECK(result->exitCode == 0);
      CHECK(result->showVersion);
    }
  }

  TEST_CASE("GtkStartupPlan - valid successor owns its private arguments and preserves GTK argument order",
            "[gtk][unit][app]")
  {
    auto const arguments = std::array<std::string_view, 7>{"aobus-gtk",
                                                           "--display=:7",
                                                           kSuccessorOption,
                                                           "--library-root=/music/../library",
                                                           kScanAfterOpenOption,
                                                           "--gtk-debug=actions",
                                                           "--name=Aobus"};

    auto result = planGtkStartup(arguments);

    REQUIRE(result);
    CHECK(result->registrationMode == GtkApplicationRegistrationMode::ReplaceExisting);
    REQUIRE(result->optSuccessorLibraryRoot);
    CHECK(*result->optSuccessorLibraryRoot == std::filesystem::path{"/library"});
    CHECK(result->scanAfterOpen);
    CHECK(result->gtkArguments ==
          std::vector<std::string>{"aobus-gtk", "--display=:7", "--gtk-debug=actions", "--name=Aobus"});
  }

  TEST_CASE("GtkStartupPlan - malformed successor arguments fail before application registration", "[gtk][unit][app]")
  {
    SECTION("library root without replacement")
    {
      auto const arguments = std::array<std::string_view, 3>{"aobus-gtk", kLibraryRootOption, "/music"};

      auto result = planGtkStartup(arguments);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::InvalidInput);
      CHECK(result.error().message.contains("must be specified together"));
    }

    SECTION("replacement without library root")
    {
      auto const arguments = std::array<std::string_view, 2>{"aobus-gtk", kSuccessorOption};

      auto result = planGtkStartup(arguments);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::InvalidInput);
      CHECK(result.error().message.contains("must be specified together"));
    }

    SECTION("duplicate replacement")
    {
      auto const arguments =
        std::array<std::string_view, 5>{"aobus-gtk", kSuccessorOption, kSuccessorOption, kLibraryRootOption, "/music"};

      auto result = planGtkStartup(arguments);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::InvalidInput);
      CHECK(result.error().message.contains("once"));
    }

    SECTION("duplicate library root")
    {
      auto const arguments = std::array<std::string_view, 6>{
        "aobus-gtk", kSuccessorOption, kLibraryRootOption, "/music", "--library-root=/other", "--debug"};

      auto result = planGtkStartup(arguments);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::InvalidInput);
      CHECK(result.error().message.contains("once"));
    }

    SECTION("missing library root value")
    {
      auto const arguments = std::array<std::string_view, 3>{"aobus-gtk", kSuccessorOption, kLibraryRootOption};

      auto result = planGtkStartup(arguments);

      REQUIRE(result);
      CHECK(result->shouldExit);
      CHECK(result->exitCode != 0);
    }

    SECTION("relative library root")
    {
      auto const arguments =
        std::array<std::string_view, 4>{"aobus-gtk", kSuccessorOption, kLibraryRootOption, "music"};

      auto result = planGtkStartup(arguments);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::InvalidInput);
      CHECK(result.error().message.contains("absolute"));
    }

    SECTION("scan intent without successor")
    {
      auto const arguments = std::array<std::string_view, 2>{"aobus-gtk", kScanAfterOpenOption};

      auto result = planGtkStartup(arguments);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::InvalidInput);
      CHECK(result.error().message.contains("requires a successor"));
    }
  }

  TEST_CASE("GtkStartupPlan - incomplete successor startup requires a native diagnostic", "[gtk][unit][app]")
  {
    CHECK_FALSE(incompleteSuccessorStartupDiagnostic(GtkApplicationRegistrationMode::AllowReplacement, false, 1));
    CHECK_FALSE(incompleteSuccessorStartupDiagnostic(GtkApplicationRegistrationMode::ReplaceExisting, true, 1));

    auto optCleanExitDiagnostic =
      incompleteSuccessorStartupDiagnostic(GtkApplicationRegistrationMode::ReplaceExisting, false, 0);
    REQUIRE(optCleanExitDiagnostic);
    CHECK(optCleanExitDiagnostic->contains("before application activation"));

    auto optFailedRegistrationDiagnostic =
      incompleteSuccessorStartupDiagnostic(GtkApplicationRegistrationMode::ReplaceExisting, false, 7);
    REQUIRE(optFailedRegistrationDiagnostic);
    CHECK(optFailedRegistrationDiagnostic->contains("exit code 7"));
  }
} // namespace ao::gtk::test
