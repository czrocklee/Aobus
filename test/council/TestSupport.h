// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "council/ProcessRunner.h"
#include "test/unit/TestFixtureSupport.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ao::council::test
{
  using ao::test::readFile;
  using ao::test::TempDir;

  inline std::filesystem::path tempPath(ao::test::TempDir const& temp)
  {
    return temp.path();
  }

  std::filesystem::path writeFile(ao::test::TempDir const& temp, std::string const& name, std::string_view content);

  std::string intentYaml(std::string_view id = "phase-a",
                         std::string_view dependency = "",
                         std::string_view path = "lib/audio/Player.cpp");

  void runCommand(ProcessRunner& process, std::filesystem::path const& cwd, std::vector<std::string> argv);

  inline void setupGitRepo(ProcessRunner& process, std::filesystem::path const& repo, std::filesystem::path const& cwd)
  {
    runCommand(process, cwd, {"git", "init", repo.string()});
  }
} // namespace ao::council::test
