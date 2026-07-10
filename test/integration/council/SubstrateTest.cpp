// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "council/Substrate.h"

#include "council/CouncilSchema.h"
#include "council/ProcessRunner.h"
#include "council/Serialization.h"
#include "test/council/TestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace ao::council::test
{
  TEST_CASE("Snapshot provider - stale base destination is replaced", "[council][integration][substrate]")
  {
    auto temp = ao::test::TempDir{};
    auto const repo = tempPath(temp) / "repo";
    std::filesystem::create_directories(repo);
    writeFile(temp, "repo/source.txt", "fresh\n");
    auto process = BoostProcessRunner{};
    setupGitRepo(process, repo, tempPath(temp));

    auto const destination = tempPath(temp) / "out" / ".base";
    writeFile(temp, "out/.base/stale.txt", "left over by a crashed run\n");

    auto snapshot = SnapshotProvider{process};
    auto base = snapshot.createImmutableBase(repo, destination);

    REQUIRE(base);
    CHECK_FALSE(std::filesystem::exists(destination / "stale.txt"));
    CHECK(std::filesystem::exists(destination / "source.txt"));
  }

  TEST_CASE("Snapshot provider - review base ref exposes pre-run uncommitted changes",
            "[council][integration][substrate]")
  {
    auto temp = ao::test::TempDir{};
    auto const repo = tempPath(temp) / "repo";
    std::filesystem::create_directories(repo);
    writeFile(temp, "repo/source.txt", "original\n");
    auto process = BoostProcessRunner{};
    setupGitRepo(process, repo, tempPath(temp));
    runCommand(process, repo, {"git", "-C", repo.string(), "config", "user.name", "Aobus Test"});
    runCommand(process, repo, {"git", "-C", repo.string(), "config", "user.email", "test@localhost"});
    runCommand(process, repo, {"git", "-C", repo.string(), "add", "source.txt"});
    runCommand(process, repo, {"git", "-C", repo.string(), "commit", "--no-gpg-sign", "-m", "initial"});
    writeFile(temp, "repo/source.txt", "changed\n");
    writeFile(temp, "repo/new.txt", "added\n");

    auto const destination = tempPath(temp) / "out" / ".base";
    auto snapshot = SnapshotProvider{process};
    auto base = snapshot.createImmutableBase(repo, destination);

    REQUIRE(base);
    auto diff = process.run(ProcessRequest{
      .argv = {"git", "-C", destination.string(), "diff", std::string{kReviewBaseRef} + "..HEAD"},
      .cwd = destination,
      .standardInput = {},
      .environmentWhitelist = {"PATH"},
      .environment = {},
      .timeout = std::chrono::seconds{10},
      .terminationGracePeriod = std::chrono::seconds{1},
    });
    REQUIRE(diff.status == ProcessStatus::Exited);
    REQUIRE(diff.exitCode == 0);
    CHECK(diff.standardOutput.contains("+changed"));
    CHECK(diff.standardOutput.contains("+added"));
  }

  TEST_CASE("Snapshot provider - stale git locks in the source do not break the base",
            "[council][integration][substrate]")
  {
    auto temp = ao::test::TempDir{};
    auto const repo = tempPath(temp) / "repo";
    std::filesystem::create_directories(repo);
    writeFile(temp, "repo/source.txt", "original\n");
    auto process = BoostProcessRunner{};
    setupGitRepo(process, repo, tempPath(temp));

    // Simulate a parent git operation interrupted mid-flight: the index lock rides into the snapshot copy
    // and would otherwise make the base's own `git add`/`commit` fail with "index.lock exists".
    writeFile(temp, "repo/.git/index.lock", "");

    auto const destination = tempPath(temp) / "out" / ".base";
    auto snapshot = SnapshotProvider{process};
    auto base = snapshot.createImmutableBase(repo, destination);

    REQUIRE(base);
    CHECK_FALSE(std::filesystem::exists(destination / ".git" / "index.lock"));
  }

  TEST_CASE("Artifact store - appends trace documents", "[council][integration][substrate]")
  {
    auto temp = ao::test::TempDir{};
    auto store = ArtifactStore{tempPath(temp)};

    REQUIRE(store.append("trace.yaml", emitTraceEvent("phase-started", {{"phase-id", "phase-a"}})));
    REQUIRE(store.append("trace.yaml", emitTraceEvent("phase-completed", {{"phase-id", "phase-a"}})));

    auto stream = readScalarStream(tempPath(temp) / "trace.yaml", "aobus-council-trace-event/v1");
    REQUIRE(stream);
    REQUIRE(stream->documents.size() == 2);
    CHECK(stream->documents.front().at("event") == "phase-started");
    CHECK(stream->documents.back().at("event") == "phase-completed");
  }
} // namespace ao::council::test
