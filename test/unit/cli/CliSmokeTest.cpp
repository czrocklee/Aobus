// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "CliTestSupport.h"
#include "Run.h"
#include "test/unit/TestUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/AppVersion.h>
#include <ao/CoreIds.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/yaml/Utils.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ao::cli::test
{
  namespace
  {
    namespace fs = std::filesystem;

    struct CliResult final
    {
      std::int32_t status = 0;
      std::string out;
      std::string err;
    };

    bool contains(std::string_view text, std::string_view expected)
    {
      return text.find(expected) != std::string_view::npos;
    }

    CliResult runArgs(std::vector<std::string> args)
    {
      auto out = std::ostringstream{};
      auto err = std::ostringstream{};
      auto const status = run(args, out, err);
      return {.status = status, .out = out.str(), .err = err.str()};
    }

    void requireJsonLineParses(std::string_view line)
    {
      auto tree = parseYaml(line);
      REQUIRE(tree.rootref().is_map());
    }

    std::size_t countJsonLinesWithField(std::string_view lines, std::string_view field, std::string_view value)
    {
      std::size_t count = 0;
      std::size_t pos = 0;

      while (pos < lines.size())
      {
        auto const end = lines.find('\n', pos);
        auto const line = lines.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);

        if (!line.empty())
        {
          auto tree = parseYaml(line);
          REQUIRE(tree.rootref().is_map());

          if (yaml::scalarView(yaml::findChild(tree.rootref(), field)) == value)
          {
            ++count;
          }
        }

        if (end == std::string_view::npos)
        {
          break;
        }

        pos = end + 1;
      }

      return count;
    }

    void checkDomainFailure(CliResult const& result, std::string_view expectedError)
    {
      CHECK(result.status == 1);
      CHECK(result.out.empty());
      CHECK(contains(result.err, expectedError));
    }

    std::size_t countOccurrences(std::string_view text, std::string_view needle)
    {
      std::size_t count = 0;
      std::size_t pos = 0;

      while ((pos = text.find(needle, pos)) != std::string_view::npos)
      {
        ++count;
        pos += needle.size();
      }

      return count;
    }

    std::uint32_t parseCreatedListId(std::string_view output)
    {
      auto constexpr kPrefix = std::string_view{"add list: "};
      auto const start = output.find(kPrefix);
      REQUIRE(start != std::string_view::npos);

      auto const idStart = start + kPrefix.size();
      auto const idEnd = output.find(' ', idStart);
      REQUIRE(idEnd != std::string_view::npos);

      return static_cast<std::uint32_t>(std::stoul(std::string{output.substr(idStart, idEnd - idStart)}));
    }

    std::uint32_t parseFirstTrackId(std::string_view output)
    {
      auto const idStart = output.find_first_of("0123456789");
      REQUIRE(idStart != std::string_view::npos);
      auto const idEnd = output.find(' ', idStart);
      REQUIRE(idEnd != std::string_view::npos);
      return static_cast<std::uint32_t>(std::stoul(std::string{output.substr(idStart, idEnd - idStart)}));
    }

    std::uint32_t parseJsonUintField(std::string_view output, std::string_view field)
    {
      auto tree = parseYaml(output);
      auto const value = yaml::scalarView(yaml::findChild(tree.rootref(), field));
      REQUIRE_FALSE(value.empty());
      return static_cast<std::uint32_t>(std::stoul(std::string{value}));
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

        ::setenv(_name.c_str(), value.string().c_str(), 1);
      }

      ~EnvVarGuard()
      {
        if (!_hadPrevious)
        {
          ::unsetenv(_name.c_str());
        }
        else
        {
          ::setenv(_name.c_str(), _previous.c_str(), 1);
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

    class CliFixture final
    {
    public:
      fs::path const& root() const { return _temp.path(); }

      void copyAudio(std::string_view sourceName, std::string_view targetName) const
      {
        fs::copy_file(
          fs::path{TAG_TEST_DATA_DIR} / sourceName, root() / targetName, fs::copy_options::overwrite_existing);
      }

      TrackId addTrack(library::test::TrackSpec const& spec) const
      {
        auto musicLibrary = library::MusicLibrary{root(), root() / ".aobus/library"};
        return library::test::addTrack(musicLibrary, spec);
      }

      ResourceId addResource(std::span<std::byte const> bytes) const
      {
        auto musicLibrary = library::MusicLibrary{root(), root() / ".aobus/library"};
        auto txn = musicLibrary.writeTransaction();
        auto idResult = musicLibrary.resources().writer(txn).create(bytes);
        REQUIRE(idResult);
        REQUIRE(txn.commit());
        return *idResult;
      }

      CliResult run(std::initializer_list<std::string_view> args) const
      {
        auto argv = std::vector<std::string>{"aobus", "-C", root().string()};
        argv.reserve(argv.size() + args.size());

        for (auto arg : args)
        {
          argv.emplace_back(arg);
        }

        return runArgs(argv);
      }

    private:
      ao::test::TempDir _temp;
    };
  } // namespace

  TEST_CASE("CLI - init and dump commands run against fixture library", "[cli][workflow][smoke]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "basic_metadata.flac");
    fixture.copyAudio("hires.flac", "hires.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "new 2  changed 0  moved 0  missing 0  unchanged 0  errors 0"));

    result = fixture.run({"init"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "new 0  changed 0  moved 0  missing 0  unchanged 2  errors 0"));

    result = fixture.run({"-O", "json", "track", "show"});
    REQUIRE(result.status == 0);
    CHECK(countOccurrences(result.out, R"("title":)") == 2);

    result = fixture.run({"track", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Test Title"));
    CHECK(contains(result.out, "HiRes Title"));

    result = fixture.run({"-O", "yaml", "track", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "tracks:"));
    CHECK(contains(result.out, "artist: \"Test Artist\""));
    CHECK(countOccurrences(result.out, "id:") == 2);

    result = fixture.run({"-O", "yaml", "track", "show", "--limit", "1"});
    REQUIRE(result.status == 0);
    CHECK(countOccurrences(result.out, "id:") == 1);
    auto const firstPage = result.out;

    result = fixture.run({"-O", "yaml", "track", "show", "--offset", "1"});
    REQUIRE(result.status == 0);
    CHECK(countOccurrences(result.out, "id:") == 1);
    CHECK(result.out != firstPage);

    result = fixture.run({"-O", "json", "track", "show", "--filter", "$title ~ \"Test\""});
    REQUIRE(result.status == 0);
    requireJsonLineParses(result.out);
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["title"]) == "Test Title");

    result = fixture.run({"track", "show", "--filter", "#NonExistentTag"});
    REQUIRE(result.status == 0);
    CHECK(result.out.empty());
    CHECK(result.err.empty());

    result = fixture.run({"track", "dump", "--id", "1"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Title:"));

    result = fixture.run({"track", "dump", "--raw"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Track ID:"));

    result = fixture.run({"-O", "yaml", "list", "dump"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "lists:"));

    result = fixture.run({"-O", "yaml", "lib", "dump", "--meta"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "meta:"));

    result = fixture.run({"lib", "dump", "--dict"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Dictionary"));

    result = fixture.run({"-O", "yaml", "lib", "dump", "--manifest"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "manifest:"));

    result = fixture.run({"lib", "dump", "--resources"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Resources"));

    result = fixture.run({"-O", "json", "lib", "dump", "--manifest"});
    REQUIRE(result.status == 0);
    auto jsonTree = parseYaml(result.out);
    REQUIRE(jsonTree.rootref().is_map());
    CHECK(jsonTree.rootref()["manifest"].is_seq());
  }

  TEST_CASE("CLI - track show format expression streams formatted rows", "[cli][workflow][track][format]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "basic_metadata.flac");
    fixture.copyAudio("hires.flac", "hires.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"track", "show", "--filter", "$title ~ \"Test\"", "--format", R"($artist + " - " + $title)"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(result.out == "Test Artist - Test Title\n");

    checkDomainFailure(fixture.run({"track", "show", "--format", "#fav"}), "format error:");
    checkDomainFailure(fixture.run({"-O", "json", "track", "show", "--format", "$title"}),
                       "track show --format supports only plain output");
  }

  TEST_CASE("CLI - track show resolves explicit id batches directly", "[cli][workflow][track][show]")
  {
    auto fixture = CliFixture{};
    auto const first = fixture.addTrack(library::test::TrackSpec{.title = "First", .uri = "first.flac"});
    fixture.addTrack(library::test::TrackSpec{.title = "Second", .uri = "second.flac"});
    auto const third = fixture.addTrack(library::test::TrackSpec{.title = "Third", .uri = "third.flac"});

    auto result = fixture.run(
      {"track", "show", std::to_string(third.raw()), std::to_string(first.raw()), std::to_string(third.raw())});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(countOccurrences(result.out, "\n") == 2);

    auto const thirdPos = result.out.find("Third");
    auto const firstPos = result.out.find("First");
    REQUIRE(thirdPos != std::string::npos);
    REQUIRE(firstPos != std::string::npos);
    CHECK(thirdPos < firstPos);
    CHECK_FALSE(contains(result.out, "Second"));
  }

  TEST_CASE("CLI - version flag reports the application version", "[cli][workflow][contract]")
  {
    auto const result = runArgs({"aobus", "--version"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, ao::kAppVersion));
  }

  TEST_CASE("CLI - track create imports one file", "[cli][workflow][track][create]")
  {
    auto fixture = CliFixture{};

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    fixture.copyAudio("basic_metadata.flac", "created.flac");
    result = fixture.run({"track", "create", (fixture.root() / "created.flac").string()});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "added track:"));

    result = fixture.run({"track", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Test Title"));
  }

  TEST_CASE("CLI - lib stats reports known fixture counts", "[cli][workflow][lib][stats]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "basic_metadata.flac");
    fixture.copyAudio("hires.flac", "hires.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"tag", "add", "fav", "1", "2"});
    REQUIRE(result.status == 0);

    result = fixture.run({"list", "create", "--name", "Pinned"});
    REQUIRE(result.status == 0);

    auto const resourceBytes = std::array{std::byte{0x01}, std::byte{0x23}, std::byte{0x45}};
    std::ignore = fixture.addResource(resourceBytes);

    result = fixture.run({"lib", "stats"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "tracks: 2"));
    CHECK(contains(result.out, "lists: 1"));
    CHECK(contains(result.out, "resources: 1"));
    CHECK(contains(result.out, "resourceBytes: 3"));
    CHECK(contains(result.out, "manifest: 2"));
    CHECK(contains(result.out, "dictionary: "));
    CHECK(contains(result.out, "tags: 1"));
    CHECK(contains(result.out, "diskBytes: "));

    result = fixture.run({"-O", "json", "lib", "stats"});
    REQUIRE(result.status == 0);
    requireJsonLineParses(result.out);
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["tracks"]) == "2");
    CHECK(yaml::scalarView(tree.rootref()["resources"]) == "1");
    CHECK(tree.rootref()["dictionary"].readable());
    CHECK(tree.rootref()["diskBytes"].readable());
  }

  TEST_CASE("CLI - lib verify reports missing files with failing exit", "[cli][workflow][lib][verify]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "track.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"lib", "verify"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "ok"));

    auto const trackPath = fixture.root() / "track.flac";
    auto const oldMtimeTime = fs::last_write_time(trackPath);
    fs::last_write_time(trackPath, oldMtimeTime + std::chrono::seconds{5});

    result = fixture.run({"lib", "verify"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "changed track.flac"));

    fs::remove(trackPath);

    result = fixture.run({"lib", "verify"});
    CHECK(result.status == 1);
    CHECK(contains(result.out, "missing track.flac"));
    CHECK(contains(result.err, "library verification failed"));
  }

  TEST_CASE("CLI - lib verify reports moved files without failing", "[cli][workflow][lib][verify]")
  {
    auto fixture = CliFixture{};
    auto const originalPath = fixture.root() / "track.flac";
    auto const movedPath = fixture.root() / "renamed.flac";
    fixture.copyAudio("basic_metadata.flac", originalPath.filename().string());

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    fs::rename(originalPath, movedPath);

    result = fixture.run({"lib", "verify"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "moved renamed.flac"));
  }

  TEST_CASE("CLI - lib relink lists, previews, and applies explicit moved-file bindings",
            "[cli][workflow][lib][relink]")
  {
    auto fixture = CliFixture{};
    auto const firstPath = fixture.root() / "first.flac";
    auto const secondPath = fixture.root() / "second.flac";
    auto const movedFirstPath = fixture.root() / "moved-first.flac";
    auto const movedSecondPath = fixture.root() / "moved-second.flac";
    fixture.copyAudio("basic_metadata.flac", firstPath.filename().string());
    fixture.copyAudio("basic_metadata.flac", secondPath.filename().string());

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    fs::rename(firstPath, movedFirstPath);
    fs::rename(secondPath, movedSecondPath);

    result = fixture.run({"lib", "relink"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "missing first.flac"));
    CHECK(contains(result.out, "new moved-first.flac"));
    CHECK(contains(result.out, "candidate first.flac -> moved-first.flac"));

    result = fixture.run({"lib", "relink", "--dry-run", "--from", "first.flac", "--to", "moved-first.flac"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "relinked first.flac -> moved-first.flac (dry-run)"));

    result = fixture.run({"-O", "json", "track", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, R"("uri": "first.flac")"));
    CHECK_FALSE(contains(result.out, R"("uri": "moved-first.flac")"));

    result = fixture.run({"lib", "relink", "--from", "first.flac", "--to", "moved-first.flac"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "relinked first.flac -> moved-first.flac"));

    result = fixture.run({"-O", "json", "track", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, R"("uri": "moved-first.flac")"));
  }

  TEST_CASE("CLI - lib relink rejects incomplete and invalid bindings", "[cli][workflow][lib][relink]")
  {
    {
      auto fixture = CliFixture{};
      auto result = fixture.run({"lib", "relink", "--from", "missing.flac"});
      checkDomainFailure(result, "lib relink requires both --from and --to");
    }

    {
      auto fixture = CliFixture{};
      fixture.copyAudio("basic_metadata.flac", "track.flac");

      auto result = fixture.run({"init"});
      REQUIRE(result.status == 0);

      result = fixture.run({"lib", "relink", "--from", "track.flac", "--to", "track.flac"});
      checkDomainFailure(result, "missing manifest row is not unresolved: track.flac");
    }

    {
      auto fixture = CliFixture{};
      auto const missingPath = fixture.root() / "missing.flac";
      auto const mismatchPath = fixture.root() / "mismatch.flac";
      fixture.copyAudio("basic_metadata.flac", missingPath.filename().string());

      auto result = fixture.run({"init"});
      REQUIRE(result.status == 0);

      fs::remove(missingPath);
      fixture.copyAudio("hires.flac", mismatchPath.filename().string());

      result = fixture.run({"lib", "relink", "--from", "missing.flac", "--to", "mismatch.flac"});
      checkDomainFailure(result, "audio identity mismatch: missing.flac -> mismatch.flac");
    }
  }

  TEST_CASE("CLI - lib resource list and export preserve raw bytes", "[cli][workflow][lib][resource]")
  {
    auto fixture = CliFixture{};
    auto const resourceBytes = std::array{std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40}};
    auto const resourceId = fixture.addResource(resourceBytes);

    auto result = fixture.run({"lib", "resource", "list"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, std::to_string(resourceId.raw())));
    CHECK(contains(result.out, "4"));

    auto const outputPath = fixture.root() / "cover.bin";
    result =
      fixture.run({"lib", "resource", "export", std::to_string(resourceId.raw()), "--output", outputPath.string()});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "exported resource:"));

    auto in = std::ifstream{outputPath, std::ios::binary};
    REQUIRE(in);
    auto const exported = std::vector<char>{std::istreambuf_iterator{in}, std::istreambuf_iterator<char>{}};
    REQUIRE(exported.size() == resourceBytes.size());

    for (std::size_t index = 0; index < resourceBytes.size(); ++index)
    {
      CHECK(std::byte{static_cast<unsigned char>(exported[index])} == resourceBytes[index]);
    }

    checkDomainFailure(
      fixture.run({"lib", "resource", "export", "999999", "--output", (fixture.root() / "missing.bin").string()}),
      "resource not found: 999999");
  }

  TEST_CASE("CLI - lib export and import round-trip library data", "[cli][workflow][lib][import-export]")
  {
    auto source = CliFixture{};
    source.copyAudio("basic_metadata.flac", "track.flac");

    auto result = source.run({"init"});
    REQUIRE(result.status == 0);

    auto const exportPath = source.root() / "library.yaml";
    result = source.run({"lib", "export", exportPath.string()});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(fs::exists(exportPath));

    auto target = CliFixture{};
    result = target.run({"-O", "json", "lib", "import", "--dry-run", exportPath.string()});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["action"]) == "import");
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "true");
    CHECK(yaml::scalarView(tree.rootref()["tracksCreated"]) == "1");

    result = target.run({"track", "show"});
    REQUIRE(result.status == 0);
    CHECK(result.out.empty());

    result = target.run({"-O", "json", "lib", "import", exportPath.string()});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "false");

    result = target.run({"track", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Test Title"));
  }

  TEST_CASE("CLI - init dry-run reports scan plan without importing tracks", "[cli][workflow][init][dryrun]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "track.flac");

    auto result = fixture.run({"init", "--dry-run"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "new 1"));

    result = fixture.run({"track", "show"});
    REQUIRE(result.status == 0);
    CHECK(result.out.empty());

    result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"track", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Test Title"));
  }

  TEST_CASE("CLI - scan applies new files and dry run preserves planned changes", "[cli][workflow][scan]")
  {
    auto fixture = CliFixture{};
    auto const trackPath = fixture.root() / "track.flac";
    fixture.copyAudio("basic_metadata.flac", trackPath.filename().string());

    auto result = fixture.run({"scan", "--dry-run"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "new 1  changed 0  moved 0  missing 0  unchanged 0  errors 0"));
    CHECK(contains(result.out, "new track.flac"));

    result = fixture.run({"track", "show"});
    REQUIRE(result.status == 0);
    CHECK(result.out.empty());

    result = fixture.run({"scan", "--verbose"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.err, "scan:"));
    CHECK(contains(result.err, "apply:"));
    CHECK(contains(result.err, "fingerprint:"));
    CHECK(contains(result.out, "new 1  changed 0  moved 0  missing 0  unchanged 0  errors 0"));

    result = fixture.run({"track", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Test Title"));

    auto const oldMtimeTime = fs::last_write_time(trackPath);
    fs::last_write_time(trackPath, oldMtimeTime + std::chrono::seconds{5});

    result = fixture.run({"scan", "--dry-run"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "new 0  changed 1  moved 0  missing 0  unchanged 0  errors 0"));
    CHECK(contains(result.out, "changed track.flac"));

    result = fixture.run({"scan", "--dry-run"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "new 0  changed 1  moved 0  missing 0  unchanged 0  errors 0"));

    fs::remove(trackPath);

    result = fixture.run({"scan", "--dry-run"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "new 0  changed 0  moved 0  missing 1  unchanged 0  errors 0"));
    CHECK(contains(result.out, "missing track.flac"));

    result = fixture.run({"scan"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "new 0  changed 0  moved 0  missing 1  unchanged 0  errors 0"));
    CHECK(contains(result.out, "1 missing file needs review"));
  }

  TEST_CASE("CLI - scan reports moved files", "[cli][workflow][scan]")
  {
    auto fixture = CliFixture{};
    auto const originalPath = fixture.root() / "track.flac";
    auto const movedPath = fixture.root() / "renamed.flac";
    fixture.copyAudio("basic_metadata.flac", originalPath.filename().string());

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    fs::rename(originalPath, movedPath);

    result = fixture.run({"scan", "--dry-run"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "new 0  changed 0  moved 1  missing 0  unchanged 0  errors 0"));
    CHECK(contains(result.out, "moved renamed.flac"));

    result = fixture.run({"scan"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "new 0  changed 0  moved 1  missing 0  unchanged 0  errors 0"));
    CHECK(contains(result.out, "Relinked 1 moved file"));

    result = fixture.run({"scan", "--dry-run"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "new 0  changed 0  moved 0  missing 0  unchanged 1  errors 0"));
  }

  TEST_CASE("CLI - tag commands mutate track tags", "[cli][workflow][tag]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "track.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"tag", "add", "fav", "1"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "added tag: fav to 1 track(s)"));

    result = fixture.run({"tag", "show", "1"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "fav"));

    result = fixture.run({"-O", "json", "tag", "show", "1"});
    REQUIRE(result.status == 0);
    requireJsonLineParses(result.out);
    auto tree = parseYaml(result.out);
    REQUIRE(tree.rootref()["tags"].is_seq());
    REQUIRE(tree.rootref()["tags"].num_children() == 1);
    CHECK(yaml::scalarView(tree.rootref()["tags"][0]) == "fav");

    result = fixture.run({"tag", "remove", "fav", "1"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "removed tag: fav from 1 track(s)"));

    result = fixture.run({"tag", "show", "1"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "no tags"));

    result = fixture.run({"tag", "remove", "missing", "1"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "removed tag: missing from 0 track(s)"));
  }

  TEST_CASE("CLI - tag list and batch targets use reader and writer contracts", "[cli][workflow][tag]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "basic_metadata.flac");
    fixture.copyAudio("hires.flac", "hires.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"tag", "add", "fav", "1", "2"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "added tag: fav to 2 track(s)"));

    result = fixture.run({"tag", "add", "chill", "1"});
    REQUIRE(result.status == 0);

    result = fixture.run({"tag", "list"});
    REQUIRE(result.status == 0);
    auto const favPos = result.out.find("fav  2");
    auto const chillPos = result.out.find("chill  1");
    REQUIRE(favPos != std::string::npos);
    REQUIRE(chillPos != std::string::npos);
    CHECK(favPos < chillPos);

    result = fixture.run({"tag", "show", "1", "2"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "fav"));
    CHECK_FALSE(contains(result.out, "chill"));

    result = fixture.run({"tag", "add", "live", "--filter", "$title ~ \"Title\""});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "added tag: live to 2 track(s)"));

    result = fixture.run({"-O", "json", "tag", "list"});
    REQUIRE(result.status == 0);
    auto tree = parseYaml(result.out);
    bool foundFav = false;
    bool foundLive = false;

    for (auto const tagNode : tree.rootref()["tags"].children())
    {
      if (yaml::scalarView(tagNode["name"]) == "fav" && yaml::scalarView(tagNode["count"]) == "2")
      {
        foundFav = true;
      }
      else if (yaml::scalarView(tagNode["name"]) == "live" && yaml::scalarView(tagNode["count"]) == "2")
      {
        foundLive = true;
      }
    }

    CHECK(foundFav);
    CHECK(foundLive);
  }

  TEST_CASE("CLI - list create and delete round-trip through the library", "[cli][workflow][list]")
  {
    auto fixture = CliFixture{};

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"list", "create", "--name", "My List"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "add list:"));
    CHECK(contains(result.out, "My List"));
    auto const listId = parseCreatedListId(result.out);

    result = fixture.run({"list", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "My List"));

    result = fixture.run({"list", "show", std::to_string(listId)});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "List ID:"));
    CHECK(contains(result.out, "My List"));
    CHECK(contains(result.out, "Tracks: 0"));

    result = fixture.run({"-O", "json", "list", "show"});
    REQUIRE(result.status == 0);
    requireJsonLineParses(result.out);
    auto tree = parseYaml(result.out);
    bool foundList = false;

    for (auto const listNode : tree.rootref()["lists"].children())
    {
      foundList = foundList || yaml::scalarView(listNode["name"]) == "My List";
    }

    CHECK(foundList);

    result = fixture.run({"list", "delete", std::to_string(listId)});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "deleted list:"));

    result = fixture.run({"list", "show"});
    REQUIRE(result.status == 0);
    CHECK_FALSE(contains(result.out, "My List"));
  }

  TEST_CASE("CLI - mutation summaries honor structured output", "[cli][workflow][output]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "track.flac");

    auto result = fixture.run({"-O", "json", "init"});
    REQUIRE(result.status == 0);
    requireJsonLineParses(result.out);
    auto initTree = parseYaml(result.out);
    CHECK(yaml::scalarView(initTree.rootref()["new"]) == "1");
    CHECK(yaml::scalarView(initTree.rootref()["dryRun"]) == "false");

    result = fixture.run({"-O", "json", "list", "create", "--name", "Machine"});
    REQUIRE(result.status == 0);
    requireJsonLineParses(result.out);
    auto createTree = parseYaml(result.out);
    CHECK(yaml::scalarView(createTree.rootref()["action"]) == "create");
    CHECK(yaml::scalarView(createTree.rootref()["name"]) == "Machine");

    result = fixture.run({"-O", "yaml", "track", "delete", "1"});
    REQUIRE(result.status == 0);
    auto deleteTree = parseYaml(result.out);
    CHECK(yaml::scalarView(deleteTree.rootref()["action"]) == "delete");
    CHECK(yaml::scalarView(deleteTree.rootref()["trackId"]) == "1");
  }

  TEST_CASE("CLI - list update and manual membership round-trip", "[cli][workflow][list]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "basic_metadata.flac");
    fixture.copyAudio("hires.flac", "hires.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"list", "create", "--name", "Manual"});
    REQUIRE(result.status == 0);
    auto const listId = parseCreatedListId(result.out);

    result = fixture.run({"list", "add", std::to_string(listId), "1", "2"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "added tracks to list:"));

    result = fixture.run({"list", "show", std::to_string(listId)});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Tracks: 2"));
    CHECK(contains(result.out, "Test Title"));
    CHECK(contains(result.out, "HiRes Title"));

    result = fixture.run({"list", "remove", std::to_string(listId), "1"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "removed tracks from list:"));

    result = fixture.run({"-O", "json", "list", "show", std::to_string(listId)});
    REQUIRE(result.status == 0);
    requireJsonLineParses(result.out);
    auto tree = parseYaml(result.out);
    REQUIRE(tree.rootref()["tracks"].is_seq());
    CHECK(tree.rootref()["tracks"].num_children() == 1);

    result = fixture.run({"list", "update", std::to_string(listId), "--name", "Pinned", "--desc", "Pinned songs"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "updated list:"));

    result = fixture.run({"list", "show", std::to_string(listId)});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Pinned"));
    CHECK(contains(result.out, "Pinned songs"));
  }

  TEST_CASE("CLI - list show resolves smart list tracks", "[cli][workflow][list]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "basic_metadata.flac");
    fixture.copyAudio("hires.flac", "hires.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"list", "create", "--name", "Smart", "--filter", "$title ~ \"Test\""});
    REQUIRE(result.status == 0);
    auto const listId = parseCreatedListId(result.out);

    result = fixture.run({"list", "show", std::to_string(listId)});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Type: smart"));
    CHECK(contains(result.out, "Tracks: 1"));
    CHECK(contains(result.out, "Test Title"));
    CHECK_FALSE(contains(result.out, "HiRes Title"));

    result = fixture.run({"list", "update", std::to_string(listId), "--filter", "$title ~ \"Title\""});
    REQUIRE(result.status == 0);

    result = fixture.run({"list", "show", std::to_string(listId)});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Tracks: 2"));
    CHECK(contains(result.out, "Test Title"));
    CHECK(contains(result.out, "HiRes Title"));

    checkDomainFailure(fixture.run({"list", "add", std::to_string(listId), "1"}), "list is not manual");
  }

  TEST_CASE("CLI - smart list detail honors parent membership", "[cli][workflow][list]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "basic_metadata.flac");
    fixture.copyAudio("hires.flac", "hires.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"track", "show", "--filter", "$title ~ \"Test\""});
    REQUIRE(result.status == 0);
    auto const testTrackId = parseFirstTrackId(result.out);

    result = fixture.run({"list", "create", "--name", "Manual"});
    REQUIRE(result.status == 0);
    auto const manualId = parseCreatedListId(result.out);

    result = fixture.run({"list", "add", std::to_string(manualId), std::to_string(testTrackId)});
    REQUIRE(result.status == 0);

    result = fixture.run({"list",
                          "create",
                          "--name",
                          "Child Smart",
                          "--parent",
                          std::to_string(manualId),
                          "--filter",
                          "$title ~ \"Title\""});
    REQUIRE(result.status == 0);
    auto const smartId = parseCreatedListId(result.out);

    result = fixture.run({"list", "show", std::to_string(smartId)});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Tracks: 1"));
    CHECK(contains(result.out, "Test Title"));
    CHECK_FALSE(contains(result.out, "HiRes Title"));
  }

  TEST_CASE("CLI - track delete removes the track from subsequent output", "[cli][workflow][track]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "track.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"track", "delete", "1"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "deleted track: 1"));

    result = fixture.run({"-O", "yaml", "track", "show"});
    REQUIRE(result.status == 0);
    CHECK_FALSE(contains(result.out, "Test Title"));

    result = fixture.run({"scan"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "new 1"));

    result = fixture.run({"track", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Test Title"));
  }

  TEST_CASE("CLI - track update edits metadata and reports no-op patches", "[cli][workflow][track][update]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "track.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"track", "update", "1", "--title", "Renamed"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "updated 1 of 1 matched track(s)"));

    result = fixture.run({"track", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Renamed"));
    CHECK_FALSE(contains(result.out, "Test Title"));

    result = fixture.run({"track", "update", "1", "--title", "Renamed"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "updated 0 of 1 matched track(s)"));
  }

  TEST_CASE("CLI - track update edits explicit id batches", "[cli][workflow][track][update]")
  {
    auto fixture = CliFixture{};
    auto const firstId = fixture.addTrack(library::test::makeEmptyTrackSpec("first.flac"));
    auto const secondId = fixture.addTrack(library::test::makeEmptyTrackSpec("second.flac"));
    auto const thirdId = fixture.addTrack(library::test::makeEmptyTrackSpec("third.flac"));

    auto result = fixture.run(
      {"track", "update", std::to_string(firstId.raw()), std::to_string(secondId.raw()), "--genre", "Classical"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "updated 2 of 2 matched track(s)"));

    result = fixture.run({"-O", "json", "track", "show", "--filter", "$genre = \"Classical\""});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(countJsonLinesWithField(result.out, "id", std::to_string(firstId.raw())) == 1);
    CHECK(countJsonLinesWithField(result.out, "id", std::to_string(secondId.raw())) == 1);
    CHECK(countJsonLinesWithField(result.out, "id", std::to_string(thirdId.raw())) == 0);
  }

  TEST_CASE("CLI - track update edits filtered batches with structured output", "[cli][workflow][track][update]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "basic_metadata.flac");
    fixture.copyAudio("hires.flac", "hires.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"-O", "json", "track", "update", "--filter", "$title ~ \"Title\"", "--artist", "Unified"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    requireJsonLineParses(result.out);
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["matched"]) == "2");
    CHECK(yaml::scalarView(tree.rootref()["updated"]) == "2");
    REQUIRE(tree.rootref()["trackIds"].is_seq());
    CHECK(tree.rootref()["trackIds"].num_children() == 2);
    CHECK(yaml::scalarView(tree.rootref()["changes"][0]["fields"][0]["field"]) == "artist");

    result = fixture.run({"-O", "json", "track", "show"});
    REQUIRE(result.status == 0);
    CHECK(countJsonLinesWithField(result.out, "artist", "Unified") == 2);

    result = fixture.run({"track", "update", "--filter", "$artist = \"Unified\"", "--artist", "Unified"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "updated 0 of 2 matched track(s)"));
  }

  TEST_CASE("CLI - track show exposes writable metadata fields", "[cli][workflow][track][output]")
  {
    auto fixture = CliFixture{};
    auto const trackId = fixture.addTrack(library::test::makeEmptyTrackSpec("track.flac"));

    auto result = fixture.run({"track",
                               "update",
                               std::to_string(trackId.raw()),
                               "--album-artist",
                               "Album Artist",
                               "--genre",
                               "Jazz",
                               "--composer",
                               "Composer",
                               "--work",
                               "Work",
                               "--movement",
                               "Finale",
                               "--year",
                               "1984",
                               "--track-number",
                               "7",
                               "--track-total",
                               "11",
                               "--disc-number",
                               "2",
                               "--disc-total",
                               "3",
                               "--movement-number",
                               "4",
                               "--movement-total",
                               "5"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());

    result = fixture.run({"-O", "json", "track", "show"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    auto tree = parseYaml(result.out);
    auto root = tree.rootref();
    CHECK(yaml::scalarView(root["albumArtist"]) == "Album Artist");
    CHECK(yaml::scalarView(root["genre"]) == "Jazz");
    CHECK(yaml::scalarView(root["composer"]) == "Composer");
    CHECK(yaml::scalarView(root["work"]) == "Work");
    CHECK(yaml::scalarView(root["movement"]) == "Finale");
    CHECK(yaml::scalarView(root["year"]) == "1984");
    CHECK(yaml::scalarView(root["trackNumber"]) == "7");
    CHECK(yaml::scalarView(root["trackTotal"]) == "11");
    CHECK(yaml::scalarView(root["discNumber"]) == "2");
    CHECK(yaml::scalarView(root["discTotal"]) == "3");
    CHECK(yaml::scalarView(root["movementNumber"]) == "4");
    CHECK(yaml::scalarView(root["movementTotal"]) == "5");

    result = fixture.run({"-O", "json", "track", "show", "--filter", "$movement = \"Finale\""});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["movement"]) == "Finale");

    result = fixture.run({"-O", "json", "track", "show", std::to_string(trackId.raw())});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["id"]) == std::to_string(trackId.raw()));

    result = fixture.run({"track", "show", std::to_string(trackId.raw()), "--format", "$genre + \": \" + $movement"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "Jazz: Finale"));

    result = fixture.run({"track", "show", std::to_string(trackId.raw()), "--filter", "$genre = \"Jazz\""});
    checkDomainFailure(result, "track show accepts either explicit ids or --filter");
  }

  TEST_CASE("CLI - missing field filters agree with structured omissions", "[cli][workflow][track][output]")
  {
    auto fixture = CliFixture{};
    fixture.addTrack(library::test::makeEmptyTrackSpec("missing.flac"));
    fixture.addTrack(library::test::TrackSpec{.title = "Known", .genre = "Known", .uri = "known.flac"});

    auto result = fixture.run({"-O", "json", "track", "show", "--filter", "not $genre?"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(countOccurrences(result.out, "\n") == 1);

    auto tree = parseYaml(result.out);
    auto root = tree.rootref();
    CHECK(yaml::scalarView(root["uri"]) == "missing.flac");
    CHECK_FALSE(root["genre"].readable());
    CHECK_FALSE(root["year"].readable());
    CHECK_FALSE(root["trackNumber"].readable());
  }

  TEST_CASE("CLI - genre repair loop converges through filters and structured output", "[cli][workflow][track][agent]")
  {
    auto fixture = CliFixture{};
    fixture.addTrack(library::test::TrackSpec{.title = "Missing One", .genre = "", .uri = "missing-one.flac"});
    fixture.addTrack(library::test::TrackSpec{.title = "Missing Two", .genre = "", .uri = "missing-two.flac"});
    fixture.addTrack(library::test::TrackSpec{.title = "Known", .genre = "Known", .uri = "known.flac"});

    auto result = fixture.run({"-O", "json", "track", "show", "--filter", "not $genre?"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(countOccurrences(result.out, "\n") == 2);
    CHECK(contains(result.out, "Missing One"));
    CHECK(contains(result.out, "Missing Two"));

    result =
      fixture.run({"-O", "json", "track", "update", "--dry-run", "--filter", "not $genre?", "--genre", "Inferred"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "true");
    CHECK(yaml::scalarView(tree.rootref()["matched"]) == "2");
    CHECK(yaml::scalarView(tree.rootref()["updated"]) == "2");
    CHECK(yaml::scalarView(tree.rootref()["changes"][0]["fields"][0]["newValue"]) == "Inferred");

    result = fixture.run({"-O", "json", "track", "show", "--filter", "not $genre?"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(countOccurrences(result.out, "\n") == 2);

    result = fixture.run({"-O", "json", "track", "update", "--filter", "not $genre?", "--genre", "Inferred"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "false");
    CHECK(yaml::scalarView(tree.rootref()["matched"]) == "2");
    CHECK(yaml::scalarView(tree.rootref()["updated"]) == "2");

    result = fixture.run({"-O", "json", "track", "show", "--filter", "not $genre?"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(result.out.empty());

    result = fixture.run({"-O", "json", "track", "show", "--filter", "$genre = \"Inferred\""});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(countJsonLinesWithField(result.out, "genre", "Inferred") == 2);
  }

  TEST_CASE("CLI - query help and errors teach filter usage", "[cli][workflow][contract]")
  {
    auto fixture = CliFixture{};
    fixture.addTrack(library::test::TrackSpec{});

    auto result = fixture.run({"track", "show", "--help"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "aobus track show 1 2 3"));
    CHECK(contains(result.out, "not $genre?"));
    CHECK(contains(result.out, "$artist + \" - \" + $title"));
    CHECK(contains(result.out, "$movement($m)"));
    CHECK(contains(result.out, "%customKey"));

    result = fixture.run({"track", "show", "--filter", "("});
    checkDomainFailure(result, "hint: expressions look like:");
    CHECK(contains(result.err, "$genre($g)"));
    CHECK(contains(result.err, "%customKey"));

    result = fixture.run({"track", "show", "--filter", "$gerne = Jazz"});
    checkDomainFailure(result, "did you mean '$genre'?");
    CHECK(contains(result.err, "available metadata fields:"));
  }

  TEST_CASE("CLI - help-all expands command tree and teaching footers", "[cli][workflow][contract]")
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

  TEST_CASE("CLI - track update sets and unsets custom metadata", "[cli][workflow][track][update]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "track.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"track", "update", "1", "--set", "mood=bright", "--set", "energy=high"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "updated 1 of 1 matched track(s)"));

    result = fixture.run({"-O", "json", "track", "show"});
    REQUIRE(result.status == 0);
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["custom"]["mood"]) == "bright");
    CHECK(yaml::scalarView(tree.rootref()["custom"]["energy"]) == "high");

    result = fixture.run({"track", "update", "1", "--unset", "mood"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "updated 1 of 1 matched track(s)"));

    result = fixture.run({"-O", "json", "track", "show"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK_FALSE(tree.rootref()["custom"]["mood"].readable());
    CHECK(yaml::scalarView(tree.rootref()["custom"]["energy"]) == "high");
  }

  TEST_CASE("CLI - track mutations support dry-run reports", "[cli][workflow][track][dryrun]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "track.flac");

    auto result = fixture.run({"-O", "json", "track", "create", "--dry-run", "track.flac"});
    REQUIRE(result.status == 0);
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["action"]) == "create");
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "true");
    CHECK_FALSE(tree.rootref()["trackId"].readable());
    CHECK(yaml::scalarView(tree.rootref()["uri"]) == "track.flac");

    result = fixture.run({"track", "show"});
    REQUIRE(result.status == 0);
    CHECK(result.out.empty());

    result = fixture.run({"-O", "json", "track", "create", "track.flac"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "false");
    CHECK(yaml::scalarView(tree.rootref()["trackId"]) == "1");

    result = fixture.run({"-O", "json", "track", "update", "--dry-run", "1", "--title", "Renamed"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "true");
    CHECK(yaml::scalarView(tree.rootref()["matched"]) == "1");
    CHECK(yaml::scalarView(tree.rootref()["changes"][0]["fields"][0]["oldValue"]) == "Test Title");
    CHECK(yaml::scalarView(tree.rootref()["changes"][0]["fields"][0]["newValue"]) == "Renamed");

    result = fixture.run({"track", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Test Title"));
    CHECK_FALSE(contains(result.out, "Renamed"));

    result = fixture.run({"-O", "json", "track", "update", "1", "--title", "Renamed"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "false");

    result = fixture.run({"track", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Renamed"));

    result = fixture.run({"-O", "json", "track", "delete", "--dry-run", "1"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "true");
    CHECK(yaml::scalarView(tree.rootref()["title"]) == "Renamed");

    result = fixture.run({"track", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Renamed"));

    result = fixture.run({"-O", "json", "track", "delete", "1"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "false");

    result = fixture.run({"track", "show"});
    REQUIRE(result.status == 0);
    CHECK(result.out.empty());
  }

  TEST_CASE("CLI - tag mutations support dry-run reports", "[cli][workflow][tag][dryrun]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "track.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"-O", "json", "tag", "add", "--dry-run", "Favorite", "1"});
    REQUIRE(result.status == 0);
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "true");
    CHECK(yaml::scalarView(tree.rootref()["changes"][0]["addedTags"][0]) == "Favorite");

    result = fixture.run({"tag", "show", "1"});
    REQUIRE(result.status == 0);
    CHECK_FALSE(contains(result.out, "Favorite"));

    result = fixture.run({"-O", "json", "tag", "add", "Favorite", "1"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "false");

    result = fixture.run({"tag", "show", "1"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Favorite"));

    result = fixture.run({"-O", "json", "tag", "remove", "--dry-run", "Favorite", "1"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "true");
    CHECK(yaml::scalarView(tree.rootref()["changes"][0]["removedTags"][0]) == "Favorite");

    result = fixture.run({"tag", "show", "1"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Favorite"));

    result = fixture.run({"-O", "json", "tag", "remove", "Favorite", "1"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "false");

    result = fixture.run({"tag", "show", "1"});
    REQUIRE(result.status == 0);
    CHECK_FALSE(contains(result.out, "Favorite"));
  }

  TEST_CASE("CLI - list mutations support dry-run reports", "[cli][workflow][list][dryrun]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "basic_metadata.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"-O", "json", "list", "create", "--dry-run", "--name", "Manual"});
    REQUIRE(result.status == 0);
    auto tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "true");
    CHECK_FALSE(tree.rootref()["listId"].readable());
    CHECK(yaml::scalarView(tree.rootref()["name"]) == "Manual");

    result = fixture.run({"list", "show"});
    REQUIRE(result.status == 0);
    CHECK_FALSE(contains(result.out, "Manual"));

    result = fixture.run({"-O", "json", "list", "create", "--name", "Manual"});
    REQUIRE(result.status == 0);
    auto const listId = parseJsonUintField(result.out, "listId");
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "false");

    result = fixture.run({"-O", "json", "list", "update", "--dry-run", std::to_string(listId), "--name", "Pinned"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "true");
    CHECK(yaml::scalarView(tree.rootref()["fields"][0]["field"]) == "name");
    CHECK(yaml::scalarView(tree.rootref()["fields"][0]["newValue"]) == "Pinned");

    result = fixture.run({"list", "show", std::to_string(listId)});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Manual"));
    CHECK_FALSE(contains(result.out, "Pinned"));

    result = fixture.run({"list", "update", std::to_string(listId), "--name", "Pinned"});
    REQUIRE(result.status == 0);

    result = fixture.run({"-O", "json", "list", "add", "--dry-run", std::to_string(listId), "1"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "true");
    CHECK(yaml::scalarView(tree.rootref()["addedTrackIds"][0]) == "1");

    result = fixture.run({"list", "show", std::to_string(listId)});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Tracks: 0"));

    result = fixture.run({"list", "add", std::to_string(listId), "1"});
    REQUIRE(result.status == 0);

    result = fixture.run({"-O", "json", "list", "remove", "--dry-run", std::to_string(listId), "1"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "true");
    CHECK(yaml::scalarView(tree.rootref()["removedTrackIds"][0]) == "1");

    result = fixture.run({"list", "show", std::to_string(listId)});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Tracks: 1"));

    result = fixture.run({"list", "remove", std::to_string(listId), "1"});
    REQUIRE(result.status == 0);

    result = fixture.run({"-O", "json", "list", "delete", "--dry-run", std::to_string(listId)});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "true");
    CHECK(yaml::scalarView(tree.rootref()["name"]) == "Pinned");

    result = fixture.run({"list", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Pinned"));

    result = fixture.run({"-O", "json", "list", "delete", std::to_string(listId)});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["dryRun"]) == "false");

    result = fixture.run({"list", "show"});
    REQUIRE(result.status == 0);
    CHECK_FALSE(contains(result.out, "Pinned"));
  }

  TEST_CASE("CLI - domain failures use stderr and exit non-zero", "[cli][workflow][contract]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "track.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    checkDomainFailure(fixture.run({"track", "show", "--filter", "("}), "filter error:");
    checkDomainFailure(fixture.run({"track", "show", "--filter", "$missingField = 1"}), "filter error:");
    checkDomainFailure(
      fixture.run({"track", "create", (fixture.root() / "missing.flac").string()}), "error adding track from:");
    checkDomainFailure(fixture.run({"track", "delete", "999"}), "track not found: 999");
    checkDomainFailure(fixture.run({"track", "update", "999", "--title", "Missing"}), "track not found: 999");
    checkDomainFailure(fixture.run({"track", "update", "1"}), "track update requires at least one field option");
    checkDomainFailure(fixture.run({"track", "update", "--title", "Missing"}), "track update requires track ids");
    checkDomainFailure(fixture.run({"tag", "add", "fav", "999"}), "track not found: 999");
    checkDomainFailure(fixture.run({"tag", "add", "fav"}), "tag command requires track ids");
    checkDomainFailure(fixture.run({"tag", "add", "fav", "--filter", "("}), "filter error:");
    checkDomainFailure(fixture.run({"tag", "show", "999"}), "track not found: 999");
    checkDomainFailure(fixture.run({"list", "delete", "999"}), "list not found: 999");
    checkDomainFailure(fixture.run({"list", "update", "999", "--name", "Missing"}), "list not found: 999");
    checkDomainFailure(fixture.run({"list", "create", "--name", "Bad", "--filter", "("}), "invalid list filter");
    checkDomainFailure(
      fixture.run({"list", "create", "--name", "Bad Parent", "--parent", "999"}), "list parent not found");

    result = fixture.run({"list", "create", "--name", "Parent"});
    REQUIRE(result.status == 0);
    auto const parentId = parseCreatedListId(result.out);
    checkDomainFailure(fixture.run({"list", "update", std::to_string(parentId), "--parent", std::to_string(parentId)}),
                       "list parent cannot be the list itself");
    checkDomainFailure(
      fixture.run({"-O", "json", "list", "dump", "--raw"}), "list dump --raw supports only plain output");
    checkDomainFailure(fixture.run({"lib", "export", (fixture.root() / "export.yaml").string(), "--mode", "bad"}),
                       "invalid export mode");
    checkDomainFailure(
      fixture.run({"lib", "export", (fixture.root() / "missing" / "export.yaml").string()}), "export failed");
    checkDomainFailure(fixture.run({"lib", "import", (fixture.root() / "import.yaml").string(), "--mode", "bad"}),
                       "invalid import mode");
    checkDomainFailure(fixture.run({"lib", "import", (fixture.root() / "missing.yaml").string()}), "import failed");
  }

  TEST_CASE("CLI - bare command groups are usage errors", "[cli][workflow][contract]")
  {
    auto fixture = CliFixture{};

    for (auto const* const group : {"track", "list", "tag", "lib"})
    {
      auto result = fixture.run({group});
      CHECK(result.status != 0);
      CHECK(result.out.empty());
    }
  }

  TEST_CASE("CLI - missing track dump id reports an error for every dump format", "[cli][workflow][contract]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "track.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    checkDomainFailure(fixture.run({"track", "dump", "--id", "999"}), "track not found: 999");
    checkDomainFailure(fixture.run({"track", "dump", "--id", "999", "--raw"}), "track not found: 999");
    checkDomainFailure(fixture.run({"-O", "yaml", "track", "dump", "--id", "999"}), "track dump supports only plain");
  }

  TEST_CASE("CLI - structured track output quotes strings and parses", "[cli][workflow][output]")
  {
    auto fixture = CliFixture{};
    auto title = std::string{"Quote \"Title\"\nSecond Line"};
    fixture.addTrack(library::test::TrackSpec{.title = title,
                                              .artist = "Artist \\ Name",
                                              .album = "Album",
                                              .uri = "special.flac",
                                              .tags = {"fav"},
                                              .customMetadata = {{"mood", "bright\nsharp"}}});

    auto result = fixture.run({"-O", "yaml", "track", "show"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    auto tree = parseYaml(result.out);
    auto tracks = tree.rootref()["tracks"];
    REQUIRE(tracks.is_seq());
    REQUIRE(tracks.num_children() == 1);
    CHECK(yaml::scalarView(tracks[0]["title"]) == title);
    CHECK(yaml::scalarView(tracks[0]["custom"]["mood"]) == "bright\nsharp");

    result = fixture.run({"-O", "json", "track", "show"});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    requireJsonLineParses(result.out);
    tree = parseYaml(result.out);
    CHECK(yaml::scalarView(tree.rootref()["title"]) == title);
    CHECK(yaml::scalarView(tree.rootref()["custom"]["mood"]) == "bright\nsharp");
  }

  TEST_CASE("CLI - empty YAML collections are sequences", "[cli][workflow][output]")
  {
    auto fixture = CliFixture{};

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    result = fixture.run({"-O", "yaml", "list", "show"});
    REQUIRE(result.status == 0);
    auto tree = parseYaml(result.out);
    REQUIRE(tree.rootref()["lists"].is_seq());
    CHECK(tree.rootref()["lists"].num_children() == 0);

    result = fixture.run({"-O", "yaml", "lib", "resource", "list"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    REQUIRE(tree.rootref()["resources"].is_seq());
    CHECK(tree.rootref()["resources"].num_children() == 0);

    result = fixture.run({"-O", "yaml", "lib", "dump", "--manifest"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    REQUIRE(tree.rootref()["manifest"].is_seq());
    CHECK(tree.rootref()["manifest"].num_children() == 0);

    result = fixture.run({"-O", "yaml", "lib", "dump", "--resources"});
    REQUIRE(result.status == 0);
    tree = parseYaml(result.out);
    REQUIRE(tree.rootref()["resources"].is_seq());
    CHECK(tree.rootref()["resources"].num_children() == 0);
  }

  TEST_CASE("CLI - root option works outside the music root", "[cli][workflow][root]")
  {
    auto fixture = CliFixture{};
    fixture.copyAudio("basic_metadata.flac", "track.flac");

    auto result = fixture.run({"init"});
    REQUIRE(result.status == 0);

    auto other = ao::test::TempDir{};
    auto currentPath = CurrentPathGuard{other.path()};

    result = runArgs({"aobus", "track", "show", "--root", fixture.root().string()});
    REQUIRE(result.status == 0);
    CHECK(result.err.empty());
    CHECK(contains(result.out, "Test Title"));
  }

  TEST_CASE("CLI - root flag overrides AOBUS_ROOT", "[cli][workflow][root]")
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

    result = runArgs({"aobus", "track", "show"});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "Test Title"));
    CHECK_FALSE(contains(result.out, "HiRes Title"));

    result = runArgs({"aobus", "track", "show", "--root", flagFixture.root().string()});
    REQUIRE(result.status == 0);
    CHECK(contains(result.out, "HiRes Title"));
    CHECK_FALSE(contains(result.out, "Test Title"));
  }
} // namespace ao::cli::test
