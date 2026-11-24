// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CliTestSupport.h"
#include "test/TestFixtureSupport.h"
#include <ao/AppVersion.h>

#include <catch2/catch_test_macros.hpp>

#ifdef _WIN32
#include <ao/utility/Path.h>
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_message.hpp>
#include <windows.h>

#include <array>
#include <iterator>
#include <string_view>
#else
#include <stdlib.h> // NOLINT(modernize-deprecated-headers) -- POSIX setenv/unsetenv require this header.
#endif

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <tuple>

namespace ao::cli::test
{
  namespace
  {
    namespace fs = std::filesystem;

#ifdef _WIN32
    constexpr auto kNamespaceRootError =
      std::string_view{"Windows namespace paths are not supported for the library root"};
    constexpr auto kInvalidRootError = std::string_view{"invalid library root"};

    void requireVolumeGuidLookup(BOOL success, char const* operation)
    {
      if (success == FALSE)
      {
        auto const error = ::GetLastError();

        if (error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_FUNCTION)
        {
          SKIP(operation << " is unsupported on the fixture volume (Win32 error " << error << ')');
        }

        FAIL(operation << " failed (Win32 error " << error << ')');
      }
    }

    fs::path volumeGuidPath(fs::path const& directory)
    {
      auto mount = std::array<wchar_t, MAX_PATH + 1>{};
      requireVolumeGuidLookup(
        ::GetVolumePathNameW(directory.c_str(), mount.data(), static_cast<DWORD>(mount.size())), "GetVolumePathNameW");

      if (::GetDriveTypeW(mount.data()) == DRIVE_REMOTE)
      {
        SKIP("A network fixture volume has no local volume GUID");
      }

      auto guid = std::array<wchar_t, MAX_PATH + 1>{};
      requireVolumeGuidLookup(
        ::GetVolumeNameForVolumeMountPointW(mount.data(), guid.data(), static_cast<DWORD>(guid.size())),
        "GetVolumeNameForVolumeMountPointW");

      auto const relative = directory.lexically_relative(fs::path{mount.data()});
      REQUIRE_FALSE(relative.empty());
      REQUIRE(relative != ".");
      REQUIRE_FALSE(relative.has_root_path());

      for (auto const& component : relative)
      {
        REQUIRE(component != "..");
      }

      auto const alias = fs::path{guid.data()} / relative;
      REQUIRE(fs::equivalent(alias, directory));
      return alias;
    }

    void requireDosFixture(fs::path const& root)
    {
      if (!root.is_absolute() || root.root_name().native().size() != 2 ||
          ::GetDriveTypeW(root.root_path().c_str()) == DRIVE_REMOTE)
      {
        SKIP("This spelling requires a local DOS drive fixture");
      }
    }
#endif

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
      EnvVarGuard(char const* name, std::optional<fs::path> const& value)
        : _name{name}
      {
        if (auto const* const previous = std::getenv(name); previous != nullptr)
        {
          _hadPrevious = true;
          _previous = previous;
        }

        if (value)
        {
          setEnvironmentVariable(_name, value->string());
        }
        else
        {
          unsetEnvironmentVariable(_name);
        }
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

  TEST_CASE("CLI - version flag reports the application version")
  {
    auto const result = runArgs({"aobus", "--version"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, ao::kAppVersion));
  }

  TEST_CASE("CLI - help-all expands command tree and teaching footers")
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

  TEST_CASE("CLI - help-all follows parser option and literal boundaries")
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

  TEST_CASE("CLI - bare command groups are usage errors")
  {
    auto fixture = CliFixture{};

    for (auto const* const group : {"track", "list", "tag", "lib"})
    {
      auto result = fixture.run({group});
      CHECK(result.status != 0);
      CHECK(result.out.empty());
    }
  }

  TEST_CASE("CLI - root option works outside the music root")
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

  TEST_CASE("CLI - rejects an empty root flag without initializing storage")
  {
    auto const fixture = CliFixture{};
    auto const envFixture = CliFixture{};
    auto const cwd = CurrentPathGuard{fixture.root()};
    auto optEnvRoot = std::optional<fs::path>{};

    SECTION("No environment root is set")
    {
    }

    SECTION("An environment root is set")
    {
      optEnvRoot = envFixture.root();
    }

    auto const env = EnvVarGuard{"AOBUS_ROOT", optEnvRoot};
    auto const result = runArgs({"aobus", "-C", "", "init"}, fixture.cacheDirectory());
    checkDomainFailure(result, "the library root must not be empty");
    CHECK(fs::is_empty(fixture.root()));
    CHECK(fs::is_empty(envFixture.root()));
  }

  TEST_CASE("CLI - uses the current directory without a nonempty environment root")
  {
    auto const fixture = CliFixture{};
    auto const cwd = CurrentPathGuard{fixture.root()};
    auto optEnvRoot = std::optional<fs::path>{};

    SECTION("The environment variable is unset")
    {
    }

#ifndef _WIN32
    // The Windows CRT deletes the variable when assigned an empty value. A true empty value needs a child environment.
    SECTION("The environment variable is empty")
    {
      optEnvRoot = fs::path{};
    }
#endif

    auto const env = EnvVarGuard{"AOBUS_ROOT", optEnvRoot};
    auto const result = runArgs({"aobus", "init"}, fixture.cacheDirectory());
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(fs::is_regular_file(fixture.root() / ".aobus/library/data.mdb"));
  }

#ifdef _WIN32
  TEST_CASE("CLI - rejects namespace Windows library roots without initializing storage")
  {
    auto fixture = CliFixture{};
    auto const& root = fixture.root();
    REQUIRE(fs::is_empty(root));
    auto rejectedRoot = fs::path{};

    SECTION("An extended DOS root is not a library root")
    {
      requireDosFixture(root);
      rejectedRoot = fs::path{LR"(\\?\)" + root.native()};
      REQUIRE(fs::equivalent(rejectedRoot, root));
    }

    SECTION("A real volume GUID alias is not a library root")
    {
      rejectedRoot = volumeGuidPath(root);
    }

    SECTION("An extended DOS prefix written with slashes is not a library root")
    {
      requireDosFixture(root);
      rejectedRoot = fs::path{L"//?/" + root.generic_wstring()};
    }

    SECTION("A device prefix on a private DOS path is not a library root")
    {
      requireDosFixture(root);
      rejectedRoot = fs::path{LR"(\\.\)" + root.native()};
    }

    auto const result = runArgs({"aobus", "-C", utility::pathToUtf8(rejectedRoot), "init"}, fixture.cacheDirectory());
    checkDomainFailure(result, kNamespaceRootError);
    // Even the storage directory and lock files must not be created on rejection.
    CHECK(fs::is_empty(root));

    fixture.copyAudio("basic_metadata.flac", "track.flac");
    auto const ordinary = runArgs({"aobus", "-C", utility::pathToUtf8(root), "init"}, fixture.cacheDirectory());
    REQUIRE(ordinary.status == 0);
    CHECK(ordinary.err.empty());
    CHECK(contains(ordinary.out, "new 1"));
  }

  TEST_CASE("CLI - rejects relative Windows library roots from a namespace current directory")
  {
    auto const fixture = CliFixture{};
    auto const& root = fixture.root();
    auto cwd = fs::path{};

    SECTION("The current directory has an extended DOS spelling")
    {
      requireDosFixture(root);
      cwd = fs::path{LR"(\\?\)" + root.native()};
      REQUIRE(fs::equivalent(cwd, root));
    }

    SECTION("The current directory has a volume GUID spelling")
    {
      cwd = volumeGuidPath(root);
    }

    auto const guard = CurrentPathGuard{cwd};
    REQUIRE(fs::current_path().native() == cwd.native());
    auto const noRoot = EnvVarGuard{"AOBUS_ROOT", std::nullopt};

    checkDomainFailure(runArgs({"aobus", "-C", ".", "init"}, fixture.cacheDirectory()), kNamespaceRootError);
    REQUIRE(fs::is_empty(root));
    checkDomainFailure(runArgs({"aobus", "init"}, fixture.cacheDirectory()), kNamespaceRootError);
    REQUIRE(fs::is_empty(root));

    auto const envRoot = EnvVarGuard{"AOBUS_ROOT", fs::path{"."}};
    checkDomainFailure(runArgs({"aobus", "init"}, fixture.cacheDirectory()), kNamespaceRootError);
    REQUIRE(fs::is_empty(root));

    auto const absolute = runArgs({"aobus", "-C", utility::pathToUtf8(root), "init"}, fixture.cacheDirectory());
    REQUIRE(absolute.status == 0);
    CHECK(absolute.err.empty());
    CHECK(fs::is_regular_file(root / ".aobus/library/data.mdb"));
  }

  TEST_CASE("CLI - rejects invalid Windows library root spellings before creating storage")
  {
    auto const fixture = CliFixture{};
    auto const& root = fixture.root();
    auto const library = root / "library";
    REQUIRE(fs::create_directory(library));
    auto const cwd = CurrentPathGuard{root};

    SECTION("A drive-relative root")
    {
      requireDosFixture(root);
      auto const driveRelative = fs::path{root.root_name().native() + L"library"};
      // If admission regresses, even this drive-relative input must stay in the fixture.
      REQUIRE(fs::equivalent(fs::absolute(driveRelative), library));
      auto const result =
        runArgs({"aobus", "-C", utility::pathToUtf8(driveRelative), "init"}, fixture.cacheDirectory());
      checkDomainFailure(result, kInvalidRootError);
    }

    SECTION("A root with an ambiguous final component")
    {
      for (auto const* supplied : {"library.", "library "})
      {
        CAPTURE(supplied);
        auto const result = runArgs({"aobus", "-C", supplied, "init"}, fixture.cacheDirectory());
        checkDomainFailure(result, kInvalidRootError);
        REQUIRE(fs::is_empty(library));
        REQUIRE(std::distance(fs::directory_iterator{root}, fs::directory_iterator{}) == 1);
      }
    }

    REQUIRE(fs::is_empty(library));
    REQUIRE(std::distance(fs::directory_iterator{root}, fs::directory_iterator{}) == 1);

    auto const ordinary = runArgs({"aobus", "-C", "library", "init"}, fixture.cacheDirectory());
    REQUIRE(ordinary.status == 0);
    CHECK(ordinary.err.empty());
    CHECK(fs::is_regular_file(library / ".aobus/library/data.mdb"));
  }

  TEST_CASE("CLI - accepts extended DOS media under an ordinary Windows library root")
  {
    auto const fixture = CliFixture{};
    auto const& root = fixture.root();
    requireDosFixture(root);
    fixture.copyAudio("basic_metadata.flac", "track.flac");
    auto const media = fs::path{LR"(\\?\)" + (root / "track.flac").native()};
    auto const rootText = utility::pathToUtf8(root);
    auto const created = runArgs(
      {"aobus", "-C", rootText, "-O", "json", "track", "create", utility::pathToUtf8(media)}, fixture.cacheDirectory());
    REQUIRE(created.status == 0);
    CHECK(created.err.empty());
    auto const creation = parseYaml(created.out);
    CHECK(yaml::scalarView(creation.rootref()["uri"]) == "track.flac");
    REQUIRE(creation.rootref().has_child("trackId"));
    auto const trackId = yaml::scalarView(creation.rootref()["trackId"]);
    REQUIRE_FALSE(trackId.empty());

    auto const reopened = runArgs({"aobus", "-C", rootText, "-O", "json", "track", "show"}, fixture.cacheDirectory());
    REQUIRE(reopened.status == 0);
    CHECK(reopened.err.empty());
    auto const view = parseYaml(reopened.out);
    CHECK(yaml::scalarView(view.rootref()["uri"]) == "track.flac");
    CHECK(yaml::scalarView(view.rootref()["id"]) == trackId);
  }

  TEST_CASE("CLI - rejects volume GUID and device namespace media paths")
  {
    auto const fixture = CliFixture{};
    auto const& root = fixture.root();
    fixture.copyAudio("basic_metadata.flac", "track.flac");
    auto media = fs::path{};
    auto error = std::string_view{};

    SECTION("A volume GUID media alias")
    {
      media = volumeGuidPath(root) / "track.flac";
      REQUIRE(fs::equivalent(media, root / "track.flac"));
      error = "Windows volume GUID paths are not supported";
    }

    SECTION("A device prefix on a private DOS media path")
    {
      requireDosFixture(root);
      media = fs::path{LR"(\\.\)" + (root / "track.flac").native()};
      error = "Invalid Windows disk namespace spelling";
    }

    auto const rejected =
      runArgs({"aobus", "-C", utility::pathToUtf8(root), "track", "create", utility::pathToUtf8(media)},
              fixture.cacheDirectory());
    checkDomainFailure(rejected, error);

    auto const rows = fixture.run({"track", "show"});
    REQUIRE(rows.status == 0);
    CHECK(rows.err.empty());
    CHECK(rows.out.empty());
  }
#endif

  TEST_CASE("CLI - root flag overrides AOBUS_ROOT")
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
