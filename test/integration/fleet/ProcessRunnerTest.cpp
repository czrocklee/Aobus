// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "fleet/ProcessRunner.h"

#include "fleet/Model.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>

namespace ao::fleet::test
{
  TEST_CASE("Fleet process runner - timeout terminates the process group", "[fleet][integration][process]")
  {
    auto runner = BoostProcessRunner{};
    auto request = ProcessRequest{
      .argv = {"sh", "-c", "trap '' TERM; sleep 30 & wait"},
      .cwd = std::filesystem::temp_directory_path(),
      .standardInput = {},
      .environmentWhitelist = {"PATH"},
      .environment = {},
      .timeout = std::chrono::milliseconds{100},
      .terminationGracePeriod = std::chrono::milliseconds{100},
    };

    auto result = runner.run(request);

    CHECK(result.status == ProcessStatus::TimedOut);
    CHECK(result.elapsed < std::chrono::seconds{5});
  }

  TEST_CASE("Fleet process runner - children get default SIGPIPE disposition", "[fleet][integration][process]")
  {
    auto runner = BoostProcessRunner{};
    // The runner ignores SIGPIPE in its own process; the launcher hook must
    // hand SIG_DFL back to the child or shell pipelines inside spawned agents
    // would silently change behavior. SIGPIPE is signal 13, bit 12 (4096) in
    // the SigIgn mask of /proc/self/status.
    auto result = runner.run(ProcessRequest{
      .argv = {"sh", "-c", "mask=$(grep SigIgn /proc/self/status | cut -f2); test $(( 0x$mask & 4096 )) -eq 0"},
      .cwd = std::filesystem::temp_directory_path(),
      .standardInput = {},
      .environmentWhitelist = {"PATH"},
      .environment = {},
      .timeout = std::chrono::seconds{5},
      .terminationGracePeriod = std::chrono::seconds{1},
    });

    CHECK(result.status == ProcessStatus::Exited);
    CHECK(result.exitCode == 0);
  }

  TEST_CASE("Fleet process runner - environment is allowlisted", "[fleet][integration][process]")
  {
    REQUIRE(::setenv("AOBUS_FLEET_SECRET", "must-not-leak", 1) == 0);
    auto runner = BoostProcessRunner{};
    auto result = runner.run(ProcessRequest{
      .argv = {"sh", "-c", "test -z \"${AOBUS_FLEET_SECRET:-}\""},
      .cwd = std::filesystem::temp_directory_path(),
      .standardInput = {},
      .environmentWhitelist = {"PATH"},
      .environment = {},
      .timeout = std::chrono::seconds{5},
      .terminationGracePeriod = std::chrono::seconds{1},
    });

    CHECK(result.status == ProcessStatus::Exited);
    CHECK(result.exitCode == 0);
    REQUIRE(::unsetenv("AOBUS_FLEET_SECRET") == 0);
  }

  TEST_CASE("Fleet process runner - launch and signal outcomes are typed", "[fleet][integration][process]")
  {
    auto runner = BoostProcessRunner{};

    auto missing = runner.run(ProcessRequest{
      .argv = {"aobus-fleet-command-that-does-not-exist"},
      .cwd = std::filesystem::temp_directory_path(),
      .standardInput = {},
      .environmentWhitelist = {"PATH"},
      .environment = {},
      .timeout = std::chrono::seconds{1},
      .terminationGracePeriod = std::chrono::seconds{1},
    });
    CHECK(missing.status == ProcessStatus::LaunchFailed);

    auto signaled = runner.run(ProcessRequest{
      .argv = {"sh", "-c", "kill -TERM $$"},
      .cwd = std::filesystem::temp_directory_path(),
      .standardInput = {},
      .environmentWhitelist = {"PATH"},
      .environment = {},
      .timeout = std::chrono::seconds{5},
      .terminationGracePeriod = std::chrono::seconds{1},
    });
    CHECK(signaled.status == ProcessStatus::Signaled);
    CHECK(signaled.signal == 15);
  }

  TEST_CASE("Fleet process runner - orphaned pipe holders do not hang the runner", "[fleet][integration][process]")
  {
    auto runner = BoostProcessRunner{};
    // The backgrounded sleep inherits the stdio pipes and outlives the shell, so the runner
    // only returns if it force-closes the pipes after the child exits.
    auto result = runner.run(ProcessRequest{
      .argv = {"sh", "-c", "sleep 30 & exit 7"},
      .cwd = std::filesystem::temp_directory_path(),
      .standardInput = {},
      .environmentWhitelist = {"PATH"},
      .environment = {},
      .timeout = std::chrono::seconds{20},
      .terminationGracePeriod = std::chrono::milliseconds{200},
    });

    CHECK(result.status == ProcessStatus::Exited);
    CHECK(result.exitCode == 7);
    CHECK(result.elapsed < std::chrono::seconds{10});
  }

  TEST_CASE("Fleet process runner - unread standard input neither kills nor hangs the runner",
            "[fleet][integration][process]")
  {
    auto runner = BoostProcessRunner{};
    // Larger than the kernel pipe buffer, so the write is still pending when the child exits.
    auto input = std::string(static_cast<std::size_t>(1) << 20, 'x');
    auto result = runner.run(ProcessRequest{
      .argv = {"sh", "-c", "exit 0"},
      .cwd = std::filesystem::temp_directory_path(),
      .standardInput = std::move(input),
      .environmentWhitelist = {"PATH"},
      .environment = {},
      .timeout = std::chrono::seconds{20},
      .terminationGracePeriod = std::chrono::seconds{2},
    });

    CHECK(result.status == ProcessStatus::Exited);
    CHECK(result.exitCode == 0);
    CHECK(result.elapsed < std::chrono::seconds{10});
  }
} // namespace ao::fleet::test
