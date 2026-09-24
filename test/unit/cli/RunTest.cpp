// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CliTestSupport.h"
#include "test/unit/TestFixtureSupport.h"
#include <ao/AppVersion.h>

#include <catch2/catch_test_macros.hpp>

#ifndef _WIN32
#include <stdlib.h> // NOLINT(modernize-deprecated-headers) -- POSIX setenv/unsetenv require this header.
#endif

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <tuple>

namespace ao::cli::test
{
  namespace
  {
    namespace fs = std::filesystem;

    void setEnvironmentVariable(std::string const& name, std::string const& value)
    {
#ifdef _WIN32
      std::ignore = ::_putenv_s(name.c_str(), value.c_str());
#else
      // Darwin's public stdlib.h forwards this declaration through private _stdlib.h.
      std::ignore = ::setenv(name.c_str(), value.c_str(), 1); // NOLINT(misc-include-cleaner)
#endif
    }

    void unsetEnvironmentVariable(std::string const& name)
    {
#ifdef _WIN32
      std::ignore = ::_putenv_s(name.c_str(), "");
#else
      // Darwin's public stdlib.h forwards this declaration through private _stdlib.h.
      std::ignore = ::unsetenv(name.c_str()); // NOLINT(misc-include-cleaner)
#endif
    }

    class [[nodiscard]] EnvVarGuard final
    {
    public:
      EnvVarGuard(char const* name, fs::path const& value)
        : _name{name}
      {
        if (auto const* const previous = std::getenv(name); previous != nullptr)
        {
          _hadPrevious = true;
          _previous = previous;
        }

        setEnvironmentVariable(_name, value.string());
      }

      ~EnvVarGuard()
      {
        if (!_hadPrevious)
        {
          unsetEnvironmentVariable(_name);
        }
        else
        {
          setEnvironmentVariable(_name, _previous);
        }
      }

      EnvVarGuard(EnvVarGuard const&) = delete;
      EnvVarGuard& operator=(EnvVarGuard const&) = delete;
      EnvVarGuard(EnvVarGuard&&) = delete;
      EnvVarGuard& operator=(EnvVarGuard&&) = delete;

    private:
      std::string _name;
      std::string _previous;
      bool _hadPrevious = false;
    };

    class [[nodiscard]] CurrentPathGuard final
    {
    public:
      explicit CurrentPathGuard(fs::path const& path)
        : _previous{fs::current_path()}
      {
        fs::current_path(path);
      }

      ~CurrentPathGuard()
      {
        auto ec = std::error_code{};
        fs::current_path(_previous, ec);
      }

      CurrentPathGuard(CurrentPathGuard const&) = delete;
      CurrentPathGuard& operator=(CurrentPathGuard const&) = delete;
      CurrentPathGuard(CurrentPathGuard&&) = delete;
      CurrentPathGuard& operator=(CurrentPathGuard&&) = delete;

    private:
      fs::path _previous;
    };
  } // namespace

  TEST_CASE("CLI - version flag reports the application version", "[cli][unit][contract]")
  {
    auto const result = runArgs({"aobus", "--version"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, ao::kAppVersion));
  }

  TEST_CASE("CLI - help-all expands command tree and teaching footers", "[cli][unit][contract]")
  {
    auto result = runArgs({"aobus", "--help-all"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "track update"));
    CHECK(contains(result.out, "list create"));
    CHECK(contains(result.out, "--dry-run"));
    CHECK(contains(result.out, "not $genre?"));
    CHECK(contains(result.out, "$artist + \" - \" + $title"));
  }

  TEST_CASE("CLI - help-all follows parser option and literal boundaries", "[cli][unit][contract]")
  {
    SECTION("A subcommand can request the recursive help tree")
    {
      auto const nested = runArgs({"aobus", "track", "--help-all"});
      REQUIRE(nested.status == 0);
      CHECK(nested.err.empty());
      CHECK(contains(nested.out, "list create"));
    }

    SECTION("An argument after the literal boundary remains an import path")
    {
      auto const fixture = CliFixture{};
      auto const literal = fixture.run({"lib", "import", "--", "--help-all"});
      CHECK(literal.status != 0);
      CHECK(contains(literal.err, "Failed to read '--help-all'"));
      CHECK_FALSE(contains(literal.out, "list create"));
    }

    SECTION("An option value does not bypass parser requirements")
    {
      auto const malformed = runArgs({"aobus", "--output", "--help-all"});
      CHECK(malformed.status != 0);
      CHECK(contains(malformed.err, "subcommand is required"));
      CHECK_FALSE(contains(malformed.out, "list create"));
    }
  }

  TEST_CASE("CLI - bare command groups are usage errors", "[cli][unit][contract]")
  {
    auto fixture = CliFixture{};

    for (auto const* const group : {"track", "list", "tag", "lib"})
    {
      auto result = fixture.run({group});
      CHECK(result.status != 0);
      CHECK(result.out.empty());
    }
  }

  TEST_CASE("CLI - root option works outside the music root", "[cli][unit][root]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "track.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    auto other = ao::test::TempDir{};
    auto currentPath = CurrentPathGuard{other.path()};

    result = runArgs({"aobus", "track", "show", "--root", fixture.root().string()}, fixture.cacheDirectory());
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "Test Title"));
  }

  TEST_CASE("CLI - root flag overrides AOBUS_ROOT", "[cli][unit][root]")
  {
    auto envFixture = CliFixture{};
    envFixture.copyAudio("basic_metadata.flac", "env.flac");
    auto result = envFixture.run({"init"});
    REQUIRE(result.status == 0);

    auto flagFixture = CliFixture{};
    flagFixture.copyAudio("hires.flac", "flag.flac");
    result = flagFixture.run({"init"});
    REQUIRE(result.status == 0);

    auto env = EnvVarGuard{"AOBUS_ROOT", envFixture.root()};

    result = runArgs({"aobus", "track", "show"}, envFixture.cacheDirectory());
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Test Title"));
    CHECK_FALSE(contains(result.out, "HiRes Title"));

    result = runArgs({"aobus", "track", "show", "--root", flagFixture.root().string()}, flagFixture.cacheDirectory());
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "HiRes Title"));
    CHECK_FALSE(contains(result.out, "Test Title"));
  }
} // namespace ao::cli::test
