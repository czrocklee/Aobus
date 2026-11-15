// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "AudioFixtureSupport.h"

#ifdef AOBUS_BAZEL_AUDIO_FIXTURES
#include "test/TestFixtureSupport.h"
#endif

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>
#ifdef AOBUS_BAZEL_AUDIO_FIXTURES
#include <stdexcept>
#include <system_error>
#endif

namespace ao::audio::test
{
  std::filesystem::path audioFixtureDirectory()
  {
#ifdef AOBUS_BAZEL_AUDIO_FIXTURES
    static std::filesystem::path const kDirectory = []
    {
      auto const marker = ao::test::resolveBazelTestRunfile("test/data/audio/basic_metadata.m4a");

      if (marker.empty())
      {
        throw std::runtime_error{"Bazel audio fixture runfile is missing: test/data/audio/basic_metadata.m4a"};
      }

      auto const markerPath = std::filesystem::path{marker};

      if (auto errorCode = std::error_code{}; !std::filesystem::is_regular_file(markerPath, errorCode))
      {
        throw std::runtime_error{"Bazel audio fixture marker is not a regular file: " + marker};
      }

      // All declared audio fixtures share this one checked-in package directory.
      return markerPath.parent_path();
    }();

    return kDirectory;
#else
    return std::filesystem::path{AUDIO_TEST_DATA_DIR};
#endif
  }

  std::filesystem::path requireAudioFixture(std::string_view fileName)
  {
    auto const path = audioFixtureDirectory() / fileName;

    INFO("Required audio fixture: " << path);
    REQUIRE(std::filesystem::is_regular_file(path));

    return path;
  }

  std::string installAudioFixture(std::filesystem::path const& libraryRoot,
                                  std::string_view fileName,
                                  std::string_view libraryUri)
  {
    auto const relativePath = std::filesystem::path{libraryUri};
    REQUIRE(relativePath.is_relative());

    auto const destination = libraryRoot / relativePath;
    std::filesystem::create_directories(destination.parent_path());
    REQUIRE(std::filesystem::is_directory(destination.parent_path()));
    REQUIRE(std::filesystem::copy_file(
      requireAudioFixture(fileName), destination, std::filesystem::copy_options::overwrite_existing));
    return relativePath.generic_string();
  }

  std::vector<std::uint8_t> readFileBytes(std::filesystem::path const& path)
  {
    auto ifs = std::ifstream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{ifs}, std::istreambuf_iterator<char>{}};
  }
} // namespace ao::audio::test
