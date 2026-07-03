// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "test/unit/TestUtils.h"
#include "test/unit/library/TrackTestSupport.h"
#include <ao/AudioScalars.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/library/MusicLibrary.h>
#include <ao/rt/library/LibraryYamlExporter.h>
#include <ao/rt/library/LibraryYamlImporter.h>
#include <ao/yaml/Utils.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  using namespace ao::library;
  namespace yaml = ao::yaml;

  namespace
  {
    ryml::Tree loadTree(std::filesystem::path const& path, std::vector<char>& buffer)
    {
      buffer = yaml::readFile(path);
      auto context = yaml::CallbackContext{path.string()};
      auto tree = ryml::Tree{yaml::callbacks(context)};
      yaml::parseInPlace(tree, buffer, context);
      tree.callbacks(yaml::callbacks());
      return tree;
    }

    struct DirectoryPermissionRestorer final
    {
      explicit DirectoryPermissionRestorer(std::filesystem::path path)
        : path{std::move(path)}
      {
      }

      DirectoryPermissionRestorer(DirectoryPermissionRestorer const&) = delete;
      DirectoryPermissionRestorer& operator=(DirectoryPermissionRestorer const&) = delete;
      DirectoryPermissionRestorer(DirectoryPermissionRestorer&&) = delete;
      DirectoryPermissionRestorer& operator=(DirectoryPermissionRestorer&&) = delete;

      ~DirectoryPermissionRestorer()
      {
        auto ec = std::error_code{};
        std::filesystem::permissions(path, std::filesystem::perms::owner_all, std::filesystem::perm_options::add, ec);
      }

      std::filesystem::path path;
    };
  } // namespace

  TEST_CASE("LibraryYaml delta export writes changed and unreadable tracks",
            "[runtime][workflow][import-export][yaml][delta]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = MusicLibrary{temp.path(), temp.path()};

    auto coverResourceId = kInvalidResourceId;
    {
      auto txn = ml.writeTransaction();
      auto result = ml.resources().writer(txn).create(std::vector{std::byte{1}, std::byte{2}, std::byte{3}});
      REQUIRE(result);
      coverResourceId = *result;
      REQUIRE(txn.commit());
    }
    library::test::addTrack(ml,
                            library::test::TrackSpec{.title = "Should Export Fully",
                                                     .artist = "",
                                                     .album = "",
                                                     .uri = "no-file.flac",
                                                     .year = 0,
                                                     .discNumber = 0,
                                                     .trackNumber = 0,
                                                     .duration = std::chrono::milliseconds{0},
                                                     .bitrate = Bitrate{},
                                                     .sampleRate = SampleRate{},
                                                     .channels = Channels{},
                                                     .bitDepth = BitDepth{}});
    library::test::addTrack(ml,
                            library::test::TrackSpec{.title = "Will fallback to full export because TagFile fails",
                                                     .artist = "",
                                                     .album = "",
                                                     .uri = "dummy.flac",
                                                     .year = 0,
                                                     .discNumber = 0,
                                                     .trackNumber = 0,
                                                     .duration = std::chrono::milliseconds{0},
                                                     .bitrate = Bitrate{},
                                                     .sampleRate = SampleRate{},
                                                     .channels = Channels{},
                                                     .bitDepth = BitDepth{}});
    library::test::addTrack(ml,
                            library::test::TrackSpec{.title = "Different Title",
                                                     .artist = "",
                                                     .album = "",
                                                     .uri = "cover.flac",
                                                     .coverArtId = coverResourceId,
                                                     .year = 0,
                                                     .discNumber = 0,
                                                     .trackNumber = 0,
                                                     .duration = std::chrono::milliseconds{0},
                                                     .bitrate = Bitrate{},
                                                     .sampleRate = SampleRate{},
                                                     .channels = Channels{},
                                                     .bitDepth = BitDepth{}});
    library::test::addTrack(ml,
                            library::test::TrackSpec{.title = "",
                                                     .artist = "",
                                                     .album = "",
                                                     .uri = "cover-removed.flac",
                                                     .year = 0,
                                                     .discNumber = 0,
                                                     .trackNumber = 0,
                                                     .duration = std::chrono::milliseconds{0},
                                                     .bitrate = Bitrate{},
                                                     .sampleRate = SampleRate{},
                                                     .channels = Channels{},
                                                     .bitDepth = BitDepth{}});

    std::filesystem::copy_file(std::filesystem::current_path() / "test/integration/tag/test_data/with_cover.flac",
                               std::filesystem::path{temp.path()} / "cover.flac");
    std::filesystem::copy_file(std::filesystem::current_path() / "test/integration/tag/test_data/with_cover.flac",
                               std::filesystem::path{temp.path()} / "cover-removed.flac");

    auto const yamlPath = std::filesystem::path{temp.path()} / "delta.yaml";
    auto exporter = LibraryYamlExporter{ml};
    REQUIRE(exporter.exportToYaml(yamlPath, rt::ExportMode::Delta));

    {
      auto buffer = std::vector<char>{};
      auto tree = loadTree(yamlPath, buffer);
      auto root = tree.rootref();
      auto tracks = root["library"]["tracks"];
      REQUIRE(tracks.is_seq());
      REQUIRE(tracks.num_children() == 4);

      CHECK(yaml::scalarView(tracks[0]["title"]) == "Should Export Fully");
      CHECK(yaml::scalarView(tracks[1]["title"]) == "Will fallback to full export because TagFile fails");
      CHECK(yaml::scalarView(tracks[2]["title"]) == "Different Title");
      CHECK(tracks[2].has_child("covers"));
      REQUIRE(tracks[3].has_child("covers"));
      CHECK(tracks[3]["covers"].is_seq());
      CHECK(tracks[3]["covers"].num_children() == 0);
      CHECK(yaml::scalarView(tracks[1]["title"]) == "Will fallback to full export because TagFile fails");
    }
  }

  TEST_CASE("LibraryYaml delta export reports filesystem inspection errors",
            "[runtime][workflow][import-export][yaml][delta][error]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = MusicLibrary{temp.path(), temp.path()};
    auto const blockedDir = std::filesystem::path{temp.path()} / "blocked";
    std::filesystem::create_directory(blockedDir);
    auto restorePermissions = DirectoryPermissionRestorer{blockedDir};

    {
      library::test::addTrack(ml,
                              library::test::TrackSpec{.title = "Cannot inspect baseline",
                                                       .artist = "",
                                                       .album = "",
                                                       .uri = "blocked/song.flac",
                                                       .year = 0,
                                                       .discNumber = 0,
                                                       .trackNumber = 0,
                                                       .duration = std::chrono::milliseconds{0},
                                                       .bitrate = Bitrate{},
                                                       .sampleRate = SampleRate{},
                                                       .channels = Channels{},
                                                       .bitDepth = BitDepth{}});
    }

    std::filesystem::permissions(blockedDir, std::filesystem::perms::none, std::filesystem::perm_options::replace);

    auto exporter = LibraryYamlExporter{ml};
    auto const result = exporter.exportToYaml(std::filesystem::path{temp.path()} / "delta.yaml", ExportMode::Delta);

    REQUIRE(!result);
    CHECK(result.error().code == Error::Code::IoError);
  }

  TEST_CASE("LibraryYaml delta import reports filesystem inspection errors",
            "[runtime][workflow][import-export][yaml][delta][error]")
  {
    auto const temp = ao::test::TempDir{};
    auto ml = MusicLibrary{temp.path(), temp.path()};
    auto importer = LibraryYamlImporter{ml};
    auto const yamlPath = std::filesystem::path{temp.path()} / "delta-import.yaml";
    auto const blockedDir = std::filesystem::path{temp.path()} / "blocked";
    std::filesystem::create_directory(blockedDir);
    auto restorePermissions = DirectoryPermissionRestorer{blockedDir};

    {
      auto yaml = std::ofstream{yamlPath};
      yaml << "version: 1\n"
           << "export_mode: delta\n"
           << "library:\n"
           << "  tracks:\n"
           << "    - uri: \"blocked/song.flac\"\n"
           << "      title: Cannot inspect baseline\n"
           << "  lists: []\n";
    }

    std::filesystem::permissions(blockedDir, std::filesystem::perms::none, std::filesystem::perm_options::replace);

    auto const result = importer.importFromYaml(yamlPath);

    REQUIRE(!result);
    CHECK(result.error().code == Error::Code::IoError);
  }
} // namespace ao::rt::test
