// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "fleet/Substrate.h"

#include "fleet/Model.h"
#include "fleet/ProcessRunner.h"
#include "test/unit/fleet/TestUtils.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string_view>

namespace ao::fleet::test
{
  TEST_CASE("Fleet snapshot provider - stale base destination is replaced", "[fleet][integration][substrate]")
  {
    auto temp = TempDir{};
    auto const repo = tempPath(temp) / "repo";
    std::filesystem::create_directories(repo);
    writeFile(temp, "repo/source.txt", "fresh\n");
    auto process = BoostProcessRunner{};
    auto init = process.run(ProcessRequest{
      .argv = {"git", "init", repo.string()},
      .cwd = tempPath(temp),
      .standardInput = {},
      .environmentWhitelist = {"PATH"},
      .environment = {},
      .timeout = std::chrono::seconds{10},
      .terminationGrace = std::chrono::seconds{1},
    });
    REQUIRE(init.exitCode == 0);

    auto const destination = tempPath(temp) / "out" / ".base";
    writeFile(temp, "out/.base/stale.txt", "left over by a crashed run\n");

    auto snapshot = SnapshotProvider{process};
    auto base = snapshot.createImmutableBase(repo, destination);

    REQUIRE(base);
    CHECK_FALSE(std::filesystem::exists(destination / "stale.txt"));
    CHECK(std::filesystem::exists(destination / "source.txt"));
  }

  TEST_CASE("Fleet patch extractor - status letters map to scope operations", "[fleet][integration][substrate]")
  {
    auto temp = TempDir{};
    auto const repo = tempPath(temp) / "repo";
    std::filesystem::create_directories(repo);
    writeFile(temp, "repo/keep.txt", "one\n");
    writeFile(temp, "repo/gone.txt", "two\n");
    auto process = BoostProcessRunner{};
    initGitRepo(process, repo, tempPath(temp));
    runCommand(process, repo, {"git", "-C", repo.string(), "add", "-A"});
    runCommand(process,
               repo,
               {"git",
                "-C",
                repo.string(),
                "-c",
                "user.name=Fleet Test",
                "-c",
                "user.email=fleet@test",
                "commit",
                "-m",
                "seed"});

    writeFile(temp, "repo/keep.txt", "one changed\n");
    std::filesystem::remove(repo / "gone.txt");
    // A created path with spaces and non-ASCII bytes exercises the NUL-delimited parsing.
    writeFile(temp, "repo/sub/añadido nuevo.txt", "three\n");

    auto extractor = PatchExtractor{process};
    auto patch = extractor.extract(repo, "candidate-z");

    REQUIRE(patch);
    REQUIRE(patch->touchedFiles.size() == 3);
    auto operationOf = [&](std::string_view path)
    {
      auto found = std::ranges::find(patch->touchedFiles, std::filesystem::path{path}, &TouchedFile::path);
      REQUIRE(found != patch->touchedFiles.end());
      return found->operation;
    };
    CHECK(operationOf("keep.txt") == ScopeOperation::Modify);
    CHECK(operationOf("gone.txt") == ScopeOperation::Delete);
    CHECK(operationOf("sub/añadido nuevo.txt") == ScopeOperation::Create);
  }
} // namespace ao::fleet::test
