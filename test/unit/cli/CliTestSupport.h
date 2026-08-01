// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "test/unit/TestFixtureSupport.h"
#include <ao/CoreIds.h>

#include <ryml.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::library::test
{
  struct TrackSpec;
}

namespace ao::cli::test
{
  struct CliResult final
  {
    std::int32_t status = 0;
    std::string out;
    std::string err;
  };

  ryml::Tree parseYaml(std::string_view text);
  bool contains(std::string_view text, std::string_view expected);
  CliResult runArgs(std::vector<std::string> args);
  void checkDomainFailure(CliResult const& result, std::string_view expectedError);
  std::size_t countOccurrences(std::string_view text, std::string_view needle);

  class CliFixture final
  {
  public:
    std::filesystem::path const& root() const;
    void copyAudio(std::string_view sourceName, std::string_view targetName) const;
    TrackId addTrack(library::test::TrackSpec const& spec) const;
    ResourceId addResource(std::span<std::byte const> bytes) const;
    CliResult run(std::initializer_list<std::string_view> args) const;

  private:
    ao::test::TempDir _temp;
  };
} // namespace ao::cli::test
