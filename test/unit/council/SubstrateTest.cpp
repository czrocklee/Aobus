// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "council/Substrate.h"

#include "council/Model.h"
#include "council/ProcessRunner.h"
#include "test/council/TestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace ao::council::test
{
  TEST_CASE("Tree canary - content mode and symlink targets affect the fingerprint", "[council][unit][canary]")
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

    SECTION("file type changes")
    {
      std::filesystem::remove(file);
      std::filesystem::create_directory(file);
      auto changed = TreeCanary::fingerprint(tempPath(temp));
      REQUIRE(changed);
      CHECK(*changed != *first);
    }
  }

  namespace
  {
    struct RecordingRunner final : ProcessRunner
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

    std::filesystem::path sandboxHomeSource(std::vector<std::string> const& argv)
    {
      for (std::size_t index = 0; index + 2 < argv.size(); ++index)
      {
        if (argv[index] == "--bind" && argv[index + 2] == "/tmp/aobus-home")
        {
          return argv[index + 1];
        }
      }

      return {};
    }

    std::string sandboxEnvValue(std::vector<std::string> const& argv, std::string const& name)
    {
      for (std::size_t index = 0; index + 2 < argv.size(); ++index)
      {
        if (argv[index] == "--setenv" && argv[index + 1] == name)
        {
          return argv[index + 2];
        }
      }

      return {};
    }
  } // namespace

  TEST_CASE("Namespace runner - sandbox mounts shape the bwrap argv", "[council][unit][substrate]")
  {
    auto recorder = RecordingRunner{};
    auto runner = NamespaceRunner{recorder};
    auto const realRepo = std::filesystem::path{"/repo/real"};
    auto const workspace = std::filesystem::path{"/work/copy"};

    SECTION("agent mounts expose HOME and bind only the workspace over the repository")
    {
      auto request = ProcessRequest{};
      request.argv = {"agent-cli"};
      [[maybe_unused]] auto const ignored = runner.run(realRepo, workspace, SandboxMounts{}, std::move(request));
      REQUIRE(recorder.requests.size() == 1);
      auto const& argv = recorder.requests.front().argv;
      CHECK(argv.front() == "bwrap");
      CHECK(std::ranges::contains(argv, std::string{"--unshare-pid"}));
      CHECK(std::ranges::contains(argv, std::string{"--unshare-ipc"}));
      CHECK(std::ranges::contains(argv, std::string{"--unshare-uts"}));
      CHECK_FALSE(containsSequence(argv, {"--ro-bind", "/", "/"}));
      CHECK_FALSE(containsSequence(argv, {"--dev-bind", "/dev", "/dev"}));
      CHECK(containsSequence(argv, {"--tmpfs", "/tmp"}));
      CHECK(containsSequence(argv, {"--dev", "/dev", "--proc", "/proc"}));
      auto const sandboxHome = sandboxHomeSource(argv);
      REQUIRE_FALSE(sandboxHome.empty());
      CHECK(std::filesystem::exists(sandboxHome));
      CHECK(containsSequence(argv, {"--setenv", "HOME", "/tmp/aobus-home"}));
      CHECK(containsSequence(argv, {"--setenv", "XDG_CONFIG_HOME", "/tmp/aobus-home/.config"}));
      CHECK(containsSequence(argv, {"--dir", "/repo"}));
      auto const workspaceBind =
        std::ranges::search(argv, std::vector<std::string>{"--bind", workspace.string(), realRepo.string()}).begin();
      auto const chdir = std::ranges::search(argv, std::vector<std::string>{"--chdir", realRepo.string()}).begin();
      CHECK(workspaceBind < chdir);
      CHECK(containsSequence(argv, {"--chdir", realRepo.string(), "agent-cli"}));
    }

    SECTION("extra writable mounts are added after the workspace")
    {
      auto mounts = SandboxMounts{.writableBinds = {{"/host/council-cache", "/tmp/build"}}};
      auto request = ProcessRequest{};
      request.argv = {"./ao", "check"};
      [[maybe_unused]] auto const ignored = runner.run(realRepo, workspace, mounts, std::move(request));
      REQUIRE(recorder.requests.size() == 1);
      auto const& argv = recorder.requests.front().argv;
      auto const workspaceBind =
        std::ranges::search(argv, std::vector<std::string>{"--bind", workspace.string(), realRepo.string()}).begin();
      auto const buildBind =
        std::ranges::search(argv, std::vector<std::string>{"--bind", "/host/council-cache", "/tmp/build"}).begin();
      CHECK(workspaceBind < buildBind);
    }

    SECTION("host HOME is mounted into the sandbox home")
    {
      auto temp = ao::test::TempDir{};
      auto const home = tempPath(temp) / "home";
      std::filesystem::create_directories(home);
      auto const* oldHomeValue = std::getenv("HOME");
      auto const oldHome = oldHomeValue == nullptr ? std::string{} : std::string{oldHomeValue};
      REQUIRE(::setenv("HOME", home.c_str(), 1) == 0);

      auto request = ProcessRequest{};
      request.argv = {"agent-cli"};
      [[maybe_unused]] auto const ignored = runner.run(realRepo, workspace, SandboxMounts{}, std::move(request));

      REQUIRE(recorder.requests.size() == 1);
      auto const& argv = recorder.requests.front().argv;
      CHECK(containsSequence(argv, {"--bind", home.string(), "/tmp/aobus-home"}));

      if (oldHomeValue == nullptr)
      {
        CHECK(::unsetenv("HOME") == 0);
      }
      else
      {
        CHECK(::setenv("HOME", oldHome.c_str(), 1) == 0);
      }
    }

    SECTION("agent executable is resolved and mounted when it comes from PATH")
    {
      auto temp = ao::test::TempDir{};
      auto const executable = writeFile(temp, "bin/agent-cli", "#!/bin/sh\n");
      auto const* oldPathValue = std::getenv("PATH");
      auto const oldPath = oldPathValue == nullptr ? std::string{} : std::string{oldPathValue};
      REQUIRE(::setenv("PATH", (tempPath(temp) / "bin").c_str(), 1) == 0);

      auto request = ProcessRequest{};
      request.argv = {"agent-cli", "--flag"};
      [[maybe_unused]] auto const ignored = runner.run(realRepo, workspace, SandboxMounts{}, std::move(request));

      REQUIRE(recorder.requests.size() == 1);
      auto const& argv = recorder.requests.front().argv;
      auto const resolved = std::filesystem::weakly_canonical(executable).string();
      CHECK(containsSequence(argv, {"--ro-bind", resolved, resolved}));
      CHECK(containsSequence(argv, {"--chdir", realRepo.string(), resolved, "--flag"}));

      if (oldPathValue == nullptr)
      {
        CHECK(::unsetenv("PATH") == 0);
      }
      else
      {
        CHECK(::setenv("PATH", oldPath.c_str(), 1) == 0);
      }
    }

    SECTION("agent executable under runtime mounts is not redundantly mounted")
    {
      auto request = ProcessRequest{};
      request.argv = {"/bin/sh", "-c", "true"};
      [[maybe_unused]] auto const ignored = runner.run(realRepo, workspace, SandboxMounts{}, std::move(request));

      REQUIRE(recorder.requests.size() == 1);
      auto const& argv = recorder.requests.front().argv;
      auto const resolved = std::filesystem::weakly_canonical("/bin/sh").string();
      CHECK_FALSE(containsSequence(argv, {"--ro-bind", resolved, resolved}));
      CHECK(containsSequence(argv, {"--chdir", realRepo.string(), resolved, "-c", "true"}));
    }

    SECTION("review tools are exposed on the sandbox path")
    {
      auto temp = ao::test::TempDir{};
      auto const git = writeFile(temp, "bin/git", "#!/bin/sh\n");
      auto const* oldPathValue = std::getenv("PATH");
      auto const oldPath = oldPathValue == nullptr ? std::string{} : std::string{oldPathValue};
      REQUIRE(::setenv("PATH", (tempPath(temp) / "bin").c_str(), 1) == 0);

      auto request = ProcessRequest{};
      request.argv = {"/bin/agent-cli"};
      [[maybe_unused]] auto const ignored = runner.run(realRepo, workspace, SandboxMounts{}, std::move(request));

      REQUIRE(recorder.requests.size() == 1);
      auto const& argv = recorder.requests.front().argv;
      auto const resolvedGit = std::filesystem::weakly_canonical(git).string();
      CHECK(containsSequence(argv, {"--dir", "/tmp/aobus-tools"}));
      CHECK(containsSequence(argv, {"--ro-bind", resolvedGit, "/tmp/aobus-tools/git"}));
      CHECK(sandboxEnvValue(argv, "PATH").starts_with("/tmp/aobus-tools:"));

      if (oldPathValue == nullptr)
      {
        CHECK(::unsetenv("PATH") == 0);
      }
      else
      {
        CHECK(::setenv("PATH", oldPath.c_str(), 1) == 0);
      }
    }
  }
} // namespace ao::council::test
