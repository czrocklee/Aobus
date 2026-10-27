// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "app/GtkStartupPlan.h"

#include <ao/Error.h>
#include <ao/desktop/LibrarySuccessorProtocol.h>
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

    auto res = planGtkStartup(arguments);

    REQUIRE(res);
    CHECK(res->registrationMode == GtkApplicationRegistrationMode::AllowReplacement);
    CHECK_FALSE(res->optSuccessorRequest);
    CHECK(res->logLevel == rt::LogLevel::Trace);
    CHECK_FALSE(res->shouldExit);
    CHECK(res->gtkArguments == std::vector<std::string>{"aobus-gtk", "--display=:7", "music.aobus", "--name=Aobus"});
  }

  TEST_CASE("GtkStartupPlan - GLib replacement option remains GTK passthrough", "[gtk][unit][app]")
  {
    auto const arguments = std::array<std::string_view, 3>{"aobus-gtk", "--gapplication-replace", "--display=:7"};

    auto res = planGtkStartup(arguments);

    REQUIRE(res);
    CHECK(res->registrationMode == GtkApplicationRegistrationMode::AllowReplacement);
    CHECK_FALSE(res->optSuccessorRequest);
    CHECK(res->gtkArguments == std::vector<std::string>{"aobus-gtk", "--gapplication-replace", "--display=:7"});
  }

  TEST_CASE("GtkStartupPlan - Aobus help and version are owned by the single CLI parser", "[gtk][unit][app]")
  {
    SECTION("help")
    {
      auto const arguments = std::array<std::string_view, 2>{"aobus-gtk", "--help"};

      auto res = planGtkStartup(arguments);

      REQUIRE(res);
      CHECK(res->shouldExit);
      CHECK(res->exitCode == 0);
      CHECK_FALSE(res->showVersion);
    }

    SECTION("version")
    {
      auto const arguments = std::array<std::string_view, 2>{"aobus-gtk", "--version"};

      auto res = planGtkStartup(arguments);

      REQUIRE(res);
      CHECK(res->shouldExit);
      CHECK(res->exitCode == 0);
      CHECK(res->showVersion);
    }
  }

  TEST_CASE("GtkStartupPlan - valid successor owns its private arguments and preserves GTK argument order",
            "[gtk][unit][app]")
  {
    auto const arguments = std::array<std::string_view, 7>{"aobus-gtk",
                                                           "--display=:7",
                                                           desktop::kLibrarySuccessorOption,
                                                           "--library-root=/music/../library",
                                                           desktop::kScanAfterOpenOption,
                                                           "--gtk-debug=actions",
                                                           "--name=Aobus"};

    auto res = planGtkStartup(arguments);

    REQUIRE(res);
    CHECK(res->registrationMode == GtkApplicationRegistrationMode::ReplaceExisting);
    REQUIRE(res->optSuccessorRequest);
    CHECK(res->optSuccessorRequest->libraryRoot == std::filesystem::path{"/library"});
    CHECK(res->optSuccessorRequest->scanAfterOpen);
    CHECK(res->gtkArguments ==
          std::vector<std::string>{"aobus-gtk", "--display=:7", "--gtk-debug=actions", "--name=Aobus"});
  }

  TEST_CASE("GtkStartupPlan - malformed successor arguments fail before application registration", "[gtk][unit][app]")
  {
    SECTION("library root without replacement")
    {
      auto const arguments = std::array<std::string_view, 3>{"aobus-gtk", desktop::kLibraryRootOption, "/music"};

      auto res = planGtkStartup(arguments);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::InvalidInput);
      CHECK(res.error().message.contains("must be specified together"));
    }

    SECTION("replacement without library root")
    {
      auto const arguments = std::array<std::string_view, 2>{"aobus-gtk", desktop::kLibrarySuccessorOption};

      auto res = planGtkStartup(arguments);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::InvalidInput);
      CHECK(res.error().message.contains("must be specified together"));
    }

    SECTION("duplicate replacement")
    {
      auto const arguments = std::array<std::string_view, 5>{"aobus-gtk",
                                                             desktop::kLibrarySuccessorOption,
                                                             desktop::kLibrarySuccessorOption,
                                                             desktop::kLibraryRootOption,
                                                             "/music"};

      auto res = planGtkStartup(arguments);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::InvalidInput);
      CHECK(res.error().message.contains("once"));
    }

    SECTION("duplicate library root")
    {
      auto const arguments = std::array<std::string_view, 6>{"aobus-gtk",
                                                             desktop::kLibrarySuccessorOption,
                                                             desktop::kLibraryRootOption,
                                                             "/music",
                                                             "--library-root=/other",
                                                             "--debug"};

      auto res = planGtkStartup(arguments);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::InvalidInput);
      CHECK(res.error().message.contains("once"));
    }

    SECTION("missing library root value")
    {
      auto const arguments =
        std::array<std::string_view, 3>{"aobus-gtk", desktop::kLibrarySuccessorOption, desktop::kLibraryRootOption};

      auto res = planGtkStartup(arguments);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::InvalidInput);
      CHECK(res.error().message.contains("requires a path"));
    }

    SECTION("relative library root")
    {
      auto const arguments = std::array<std::string_view, 4>{
        "aobus-gtk", desktop::kLibrarySuccessorOption, desktop::kLibraryRootOption, "music"};

      auto res = planGtkStartup(arguments);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::InvalidInput);
      CHECK(res.error().message.contains("absolute"));
    }

    SECTION("scan intent without successor")
    {
      auto const arguments = std::array<std::string_view, 2>{"aobus-gtk", desktop::kScanAfterOpenOption};

      auto res = planGtkStartup(arguments);

      REQUIRE_FALSE(res);
      CHECK(res.error().code == Error::Code::InvalidInput);
      CHECK(res.error().message.contains("requires a successor"));
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
