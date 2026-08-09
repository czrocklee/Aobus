// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/cli/CliTestSupport.h"

#include "Run.h"
#include "test/unit/library/MusicLibraryTestSupport.h"
#include "test/unit/library/TrackTestSupport.h"
#include "test/unit/library/WritableLibraryTestSupport.h"
#include <ao/CoreIds.h>
#include <ao/library/MusicLibrary.h>
#include <ao/library/ResourceStore.h>
#include <ao/rt/library/LibraryPaths.h>
#include <ao/yaml/RymlAdapter.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <initializer_list>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace ao::cli::test
{
  ryml::Tree parseYaml(std::string_view text)
  {
    auto state = yaml::ErrorCallbackState{};
    auto tree = ryml::Tree{yaml::callbacks()};
    REQUIRE(yaml::parseInArena(tree, text, state));
    return tree;
  }

  bool contains(std::string_view text, std::string_view expected)
  {
    return text.contains(expected);
  }

  CliResult runArgs(std::vector<std::string> args)
  {
    auto out = std::ostringstream{};
    auto err = std::ostringstream{};
    auto const status =
      run(args, out, err, CliRunOptions{.musicLibraryMapSize = library::test::kTestMusicLibraryMapSize});
    return {.status = status, .out = out.str(), .err = err.str()};
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
    std::size_t searchPosition = 0;

    while ((searchPosition = text.find(needle, searchPosition)) != std::string_view::npos)
    {
      ++count;
      searchPosition += needle.size();
    }

    return count;
  }

  std::filesystem::path const& CliFixture::root() const
  {
    return _temp.path();
  }

  void CliFixture::copyAudio(std::string_view sourceName, std::string_view targetName) const
  {
    std::filesystem::copy_file(std::filesystem::path{AUDIO_TEST_DATA_DIR} / sourceName,
                               root() / targetName,
                               std::filesystem::copy_options::overwrite_existing);
  }

  TrackId CliFixture::addTrack(library::test::TrackSpec const& spec) const
  {
    auto musicLibrary = library::test::makeTestMusicLibrary(root(), rt::LibraryPaths{root()}.databasePath());
    return library::test::addTrackWithUniqueFixtureUri(musicLibrary, spec);
  }

  ResourceId CliFixture::addResource(std::span<std::byte const> bytes) const
  {
    auto musicLibrary = library::test::makeTestMusicLibrary(root(), rt::LibraryPaths{root()}.databasePath());
    auto transaction = library::test::writeTransaction(musicLibrary);
    auto idRes = library::test::physicalWriter(musicLibrary.resources(), transaction).create(bytes);
    REQUIRE(idRes);
    REQUIRE(transaction.commit());
    return *idRes;
  }

  CliResult CliFixture::run(std::initializer_list<std::string_view> args) const
  {
    auto argv = std::vector<std::string>{"aobus", "-C", root().string()};
    argv.reserve(argv.size() + args.size());

    for (auto argument : args)
    {
      argv.emplace_back(argument);
    }

    auto result = runArgs(argv);

    if (result.status != 0)
    {
      UNSCOPED_INFO("CLI stderr: " << result.err);
    }

    return result;
  }
} // namespace ao::cli::test
