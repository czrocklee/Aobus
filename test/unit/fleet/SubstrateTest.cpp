// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "fleet/Substrate.h"

#include "fleet/Model.h"
#include "fleet/ProcessRunner.h"
#include "test/fleet/FleetTestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace ao::fleet::test
{
  TEST_CASE("Fleet tree canary - content mode and symlink targets affect the fingerprint", "[fleet][unit][canary]")
  {
    auto temp = ao::test::TempDir{};
    auto const file = writeFile(temp, "source.txt", "first\n");
    std::filesystem::create_symlink("source.txt", tempPath(temp) / "link");
    auto first = TreeCanary::fingerprint(tempPath(temp));
    REQUIRE(first);

    SECTION("file content changes")
    {
      writeFile(temp, "source.txt", "second\n");
      auto changed = TreeCanary::fingerprint(tempPath(temp));
      REQUIRE(changed);
      CHECK(*changed != *first);
    }

    SECTION("file mode changes")
    {
      std::filesystem::permissions(file, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
      auto changed = TreeCanary::fingerprint(tempPath(temp));
      REQUIRE(changed);
      CHECK(*changed != *first);
    }

    SECTION("symlink target changes")
    {
      std::filesystem::remove(tempPath(temp) / "link");
      std::filesystem::create_symlink("missing.txt", tempPath(temp) / "link");
      auto changed = TreeCanary::fingerprint(tempPath(temp));
      REQUIRE(changed);
      CHECK(*changed != *first);
    }
  }

  namespace
  {
    struct RecordingRunner final : IProcessRunner
    {
      std::vector<ProcessRequest> requests;
      ProcessResult result;

      RecordingRunner()
      {
        result.status = ProcessStatus::Exited;
        result.exitCode = 0;
      }

      ProcessResult run(ProcessRequest const& request) override
      {
        requests.push_back(request);
        return result;
      }
    };

    bool containsSequence(std::vector<std::string> const& argv, std::vector<std::string> const& sequence)
    {
      return std::ranges::search(argv, sequence).begin() != argv.end();
    }
  } // namespace

  TEST_CASE("Fleet namespace runner - sandbox mounts shape the bwrap argv", "[fleet][unit][substrate]")
  {
    auto recorder = RecordingRunner{};
    auto runner = NamespaceRunner{recorder};
    auto const realRepo = std::filesystem::path{"/repo/real"};
    auto const workspace = std::filesystem::path{"/work/copy"};
    auto const* home = std::getenv("HOME");
    REQUIRE(home != nullptr);

    SECTION("agent mounts bind HOME before the workspace")
    {
      auto request = ProcessRequest{};
      request.argv = {"agent-cli"};
      [[maybe_unused]] auto const ignored =
        runner.run(realRepo, workspace, SandboxMounts{.writableBinds = {}, .bindHome = true}, std::move(request));
      REQUIRE(recorder.requests.size() == 1);
      auto const& argv = recorder.requests.front().argv;
      CHECK(containsSequence(argv, {"--bind", home, home}));
      auto const homeBind = std::ranges::search(argv, std::vector<std::string>{"--bind", home, home}).begin();
      auto const workspaceBind =
        std::ranges::search(argv, std::vector<std::string>{"--bind", workspace.string(), realRepo.string()}).begin();
      CHECK(homeBind < workspaceBind);
      CHECK_FALSE(std::ranges::contains(argv, std::string{"--unshare-net"}));
    }

    SECTION("oracle mounts add writable binds after the workspace and never bind HOME")
    {
      auto mounts = SandboxMounts{.writableBinds = {{"/host/oracle-build", "/tmp/build"}}, .bindHome = false};
      auto request = ProcessRequest{};
      request.argv = {"./ao", "check"};
      [[maybe_unused]] auto const ignored = runner.run(realRepo, workspace, mounts, std::move(request));
      REQUIRE(recorder.requests.size() == 1);
      auto const& argv = recorder.requests.front().argv;
      CHECK_FALSE(containsSequence(argv, {"--bind", home, home}));
      auto const workspaceBind =
        std::ranges::search(argv, std::vector<std::string>{"--bind", workspace.string(), realRepo.string()}).begin();
      auto const buildBind =
        std::ranges::search(argv, std::vector<std::string>{"--bind", "/host/oracle-build", "/tmp/build"}).begin();
      CHECK(workspaceBind < buildBind);
    }
  }
} // namespace ao::fleet::test
