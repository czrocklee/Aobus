// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/utility/PlatformDirectories.h>

// Only the Windows fixture can arrange a session with no location at all: on
// POSIX the account entry always answers, so the error code is never reached.
#ifdef _WIN32
#include <ao/Error.h>
#endif

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <tuple>

namespace ao::utility::test
{
  namespace
  {
#ifdef _WIN32
    constexpr auto kExpectedDirectoryName = "Aobus";

    /**
     * @name Absolute roots for the host under test
     *
     * A Windows path needs a drive to be absolute: `/tmp/x` names a directory
     * on whichever drive is current, so the resolver treats it as relative and
     * falls through. These fixtures must therefore be spelled per platform.
     * @{
     */
    constexpr auto kPrimaryRoot = "C:\\aobus-config-primary";
    constexpr auto kFallbackRoot = "C:\\aobus-config-fallback";
    constexpr auto kHomeRoot = "C:\\aobus-home";
#else
    constexpr auto kExpectedDirectoryName = "aobus";
    constexpr auto kPrimaryRoot = "/tmp/aobus-config-primary";
    constexpr auto kHomeRoot = "/tmp/aobus-home";
#endif
    /// @}

    /// Restores the process environment the test found, so ordering stays irrelevant.
    class ScopedEnvironment final
    {
    public:
      explicit ScopedEnvironment(char const* name)
        : _name{name}
      {
        // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test scope.
        if (auto const* const value = std::getenv(name); value != nullptr)
        {
          _optOriginal = std::string{value};
        }
      }

      ScopedEnvironment(ScopedEnvironment const&) = delete;
      ScopedEnvironment& operator=(ScopedEnvironment const&) = delete;
      ScopedEnvironment(ScopedEnvironment&&) = delete;
      ScopedEnvironment& operator=(ScopedEnvironment&&) = delete;

      ~ScopedEnvironment()
      {
        if (_optOriginal)
        {
          set(_optOriginal->c_str());
        }
        else
        {
          clear();
        }
      }

      void set(char const* value) const
      {
#ifdef _WIN32
        std::ignore = ::_putenv_s(_name, value);
#else
        // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test scope.
        std::ignore = ::setenv(_name, value, 1);
#endif
      }

      void clear() const
      {
#ifdef _WIN32
        std::ignore = ::_putenv_s(_name, "");
#else
        // NOLINTNEXTLINE(concurrency-mt-unsafe): single-threaded test scope.
        std::ignore = ::unsetenv(_name);
#endif
      }

    private:
      char const* _name;
      std::optional<std::string> _optOriginal{};
    };
  } // namespace

  TEST_CASE("applicationConfigDirectory - prefers the primary variable", "[utility][unit][paths]")
  {
#ifdef _WIN32
    auto const primary = ScopedEnvironment{"LOCALAPPDATA"};
#else
    auto const primary = ScopedEnvironment{"XDG_CONFIG_HOME"};
#endif
    primary.set(kPrimaryRoot);

    auto const result = applicationConfigDirectory();

    REQUIRE(result);
    CHECK(*result == std::filesystem::path{kPrimaryRoot} / kExpectedDirectoryName);
  }

  TEST_CASE("applicationConfigDirectory - falls back when the primary variable is unset", "[utility][unit][paths]")
  {
#ifdef _WIN32
    auto const primary = ScopedEnvironment{"LOCALAPPDATA"};
    auto const fallback = ScopedEnvironment{"APPDATA"};
    primary.clear();
    fallback.set(kFallbackRoot);

    auto const result = applicationConfigDirectory();

    REQUIRE(result);
    CHECK(*result == std::filesystem::path{kFallbackRoot} / kExpectedDirectoryName);
#else
    auto const primary = ScopedEnvironment{"XDG_CONFIG_HOME"};
    auto const home = ScopedEnvironment{"HOME"};
    primary.clear();
    home.set(kHomeRoot);

    auto const result = applicationConfigDirectory();

    REQUIRE(result);
    CHECK(*result == std::filesystem::path{kHomeRoot} / ".config" / kExpectedDirectoryName);
#endif
  }

  TEST_CASE("applicationConfigDirectory - an empty variable is treated as unset", "[utility][unit][paths]")
  {
#ifdef _WIN32
    auto const primary = ScopedEnvironment{"LOCALAPPDATA"};
    auto const fallback = ScopedEnvironment{"APPDATA"};
    primary.set("");
    fallback.set(kFallbackRoot);
#else
    auto const primary = ScopedEnvironment{"XDG_CONFIG_HOME"};
    auto const home = ScopedEnvironment{"HOME"};
    primary.set("");
    home.set(kHomeRoot);
#endif

    auto const result = applicationConfigDirectory();

    REQUIRE(result);
    CHECK(result->empty() == false);
  }

  TEST_CASE("applicationConfigDirectory - a relative variable is treated as unset", "[utility][unit][paths]")
  {
    // A relative value resolves against the working directory, so honoring one
    // would put configuration, layout presets, and user CSS wherever the
    // process happened to be launched from. Every candidate must be absolute.
#ifdef _WIN32
    auto const primary = ScopedEnvironment{"LOCALAPPDATA"};
    auto const fallback = ScopedEnvironment{"APPDATA"};
    fallback.set(kFallbackRoot);
    auto const expected = std::filesystem::path{kFallbackRoot} / kExpectedDirectoryName;

    // A rooted path with no drive - `/tmp/x` - resolves against whichever drive
    // is current, and `C:config` against that drive's current directory. Both
    // are as unusable as a plain relative value.
    for (auto const* const relative : {"config", "..\\config", "C:config", "/tmp/aobus"})
#else
    auto const primary = ScopedEnvironment{"XDG_CONFIG_HOME"};
    auto const fallback = ScopedEnvironment{"HOME"};
    fallback.set(kHomeRoot);
    auto const expected = std::filesystem::path{kHomeRoot} / ".config" / kExpectedDirectoryName;

    for (auto const* const relative : {"config", "../config", "./"})
#endif
    {
      INFO(relative);
      primary.set(relative);

      auto const result = applicationConfigDirectory();

      REQUIRE(result);
      CHECK(*result == expected);
    }
  }

#ifdef _WIN32

  TEST_CASE("applicationConfigDirectory - reports NotFound when nothing names a location", "[utility][unit][paths]")
  {
    auto const primary = ScopedEnvironment{"LOCALAPPDATA"};
    auto const fallback = ScopedEnvironment{"APPDATA"};
    primary.clear();
    fallback.clear();

    auto const result = applicationConfigDirectory();

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::NotFound);
  }
#else

  TEST_CASE("applicationConfigDirectory - falls back to the account entry without HOME", "[utility][unit][paths]")
  {
    // A service-style or `env -i` session exports no HOME. GLib consults the
    // password database there, and GTK reaches this helper through the same
    // entry point, so the account's home directory has to keep working.
    auto const primary = ScopedEnvironment{"XDG_CONFIG_HOME"};
    auto const home = ScopedEnvironment{"HOME"};
    primary.clear();
    home.clear();

    auto const result = applicationConfigDirectory();

    REQUIRE(result);
    CHECK(result->filename() == kExpectedDirectoryName);
    CHECK(result->parent_path().filename() == ".config");
  }
#endif
} // namespace ao::utility::test
