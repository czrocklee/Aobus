// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "fleet/Model.h"
#include "fleet/ProcessRunner.h"
#include "test/unit/TestUtils.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::fleet::test
{
  using ao::test::TempDir;

  inline std::filesystem::path tempPath(TempDir const& temp) { return temp.path(); }

  inline std::filesystem::path writeFile(TempDir const& temp, std::string const& name, std::string_view content)
  {
    auto const result = tempPath(temp) / name;
    std::filesystem::create_directories(result.parent_path());
    auto output = std::ofstream{result, std::ios::binary | std::ios::trunc};
    output << content;
    return result;
  }

  inline std::string intentYaml(std::string_view id = "phase-a",
                                std::string_view dependency = "",
                                std::string_view path = "lib/audio/Player.cpp")
  {
    auto depends = dependency.empty() ? "[]" : std::format("[{}]", dependency);
    return std::format(R"(schema: aobus-fleet-intent/v1
id: {}
task-kind: implement-plan
invariant: Preserve behavior.
scope:
  - path: {}
    operations: [modify]
depends-on: {}
overrides: {{}}
body: |
  Implement the approved change.
)",
                       id,
                       path,
                       depends);
  }

  inline std::string readFile(std::filesystem::path const& path)
  {
    auto input = std::ifstream{path, std::ios::binary};
    return {std::istreambuf_iterator{input}, std::istreambuf_iterator<char>{}};
  }

  inline void runCommand(IProcessRunner& process, std::filesystem::path const& cwd, std::vector<std::string> argv)
  {
    auto result = process.run(ProcessRequest{
      .argv = std::move(argv),
      .cwd = cwd,
      .standardInput = {},
      .environmentWhitelist = {"PATH"},
      .environment = {},
      .timeout = std::chrono::seconds{10},
      .terminationGrace = std::chrono::seconds{1},
    });
    REQUIRE(result.status == ProcessStatus::Exited);
    REQUIRE(result.exitCode == 0);
  }

  inline void initGitRepo(IProcessRunner& process,
                          std::filesystem::path const& repo,
                          std::filesystem::path const& cwd)
  {
    runCommand(process, cwd, {"git", "init", repo.string()});
  }
} // namespace ao::fleet::test
