// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "council/ProcessRunner.h"

#include "council/CouncilSchema.h"
#include "test/unit/TestUtils.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>

namespace ao::council::test
{
  TEST_CASE("Process runner - timeout terminates the process group", "[council][integration][process]")
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

  TEST_CASE("Process runner - children get default SIGPIPE disposition", "[council][integration][process]")
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

  TEST_CASE("Process runner - environment is allowlisted", "[council][integration][process]")
  {
    REQUIRE(::setenv("AOBUS_COUNCIL_SECRET", "must-not-leak", 1) == 0);
    auto runner = BoostProcessRunner{};
    auto result = runner.run(ProcessRequest{
      .argv = {"sh", "-c", "test -z \"${AOBUS_COUNCIL_SECRET:-}\""},
      .cwd = std::filesystem::temp_directory_path(),
      .standardInput = {},
      .environmentWhitelist = {"PATH"},
      .environment = {},
      .timeout = std::chrono::seconds{5},
      .terminationGracePeriod = std::chrono::seconds{1},
    });

    CHECK(result.status == ProcessStatus::Exited);
    CHECK(result.exitCode == 0);
    CHECK(::unsetenv("AOBUS_COUNCIL_SECRET") == 0);
  }

  TEST_CASE("Process runner - launch and signal outcomes are typed", "[council][integration][process]")
  {
    auto runner = BoostProcessRunner{};

    auto missing = runner.run(ProcessRequest{
      .argv = {"aobus-council-command-that-does-not-exist"},
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

  TEST_CASE("Process runner - orphaned pipe holders do not hang the runner", "[council][integration][process]")
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

  TEST_CASE("Process runner - sink holds streamed output after a timeout kill", "[council][integration][process]")
  {
    auto temp = ao::test::TempDir{};
    auto const sinkPath = temp.path() / "logs" / "stdout.txt";
    auto runner = BoostProcessRunner{};
    auto result = runner.run(ProcessRequest{
      .argv = {"sh", "-c", "echo live-marker; sleep 30"},
      .cwd = std::filesystem::temp_directory_path(),
      .standardInput = {},
      .environmentWhitelist = {"PATH"},
      .environment = {},
      .timeout = std::chrono::milliseconds{500},
      .terminationGracePeriod = std::chrono::milliseconds{100},
      .optStdoutSink = StreamSink{.path = sinkPath},
    });

    CHECK(result.status == ProcessStatus::TimedOut);
    // The marker was flushed to disk while the child was still alive, so the kill
    // cannot lose it; the in-memory capture stays authoritative alongside.
    CHECK(ao::test::readFile(sinkPath) == "live-marker\n");
    CHECK(result.standardOutput == "live-marker\n");
  }

  TEST_CASE("Process runner - captured output is bounded", "[council][integration][process]")
  {
    auto temp = ao::test::TempDir{};
    auto const sinkPath = temp.path() / "stdout.txt";
    auto runner = BoostProcessRunner{};
    auto result = runner.run(ProcessRequest{
      .argv = {"sh", "-c", "printf 0123456789"},
      .cwd = std::filesystem::temp_directory_path(),
      .standardInput = {},
      .environmentWhitelist = {"PATH"},
      .environment = {},
      .timeout = std::chrono::seconds{5},
      .terminationGracePeriod = std::chrono::seconds{1},
      .maxCapturedBytes = 5,
      .optStdoutSink = StreamSink{.path = sinkPath},
    });

    CHECK(result.status == ProcessStatus::Exited);
    CHECK(result.exitCode == 0);
    CHECK(result.standardOutputTruncated);
    CHECK(result.standardOutput == "01234\n[... output truncated by aobus-council ...]\n");
    CHECK(ao::test::readFile(sinkPath) == result.standardOutput);
  }

  TEST_CASE("Process runner - truncation keeps UTF-8 characters whole", "[council][integration][process]")
  {
    // The 2-byte character (C3 A9) starts at byte 4 but only one byte of budget
    // remains, so the seam must drop the whole partial character, not split it.
    auto temp = ao::test::TempDir{};
    auto const sinkPath = temp.path() / "stdout.txt";
    auto runner = BoostProcessRunner{};
    auto result = runner.run(ProcessRequest{
      .argv = {"sh", "-c", "printf '0123\\303\\251xyz'"},
      .cwd = std::filesystem::temp_directory_path(),
      .standardInput = {},
      .environmentWhitelist = {"PATH"},
      .environment = {},
      .timeout = std::chrono::seconds{5},
      .terminationGracePeriod = std::chrono::seconds{1},
      .maxCapturedBytes = 5,
      .optStdoutSink = StreamSink{.path = sinkPath},
    });

    CHECK(result.status == ProcessStatus::Exited);
    CHECK(result.standardOutputTruncated);
    CHECK(result.standardOutput == "0123\n[... output truncated by aobus-council ...]\n");
    CHECK(ao::test::readFile(sinkPath) == result.standardOutput);
  }

  TEST_CASE("Process runner - append sinks accumulate and fresh sinks truncate", "[council][integration][process]")
  {
    auto temp = ao::test::TempDir{};
    auto const sinkPath = temp.path() / "combined.log";
    auto runner = BoostProcessRunner{};
    auto request = ProcessRequest{
      .argv = {"sh", "-c", "echo first"},
      .cwd = std::filesystem::temp_directory_path(),
      .standardInput = {},
      .environmentWhitelist = {"PATH"},
      .environment = {},
      .timeout = std::chrono::seconds{5},
      .terminationGracePeriod = std::chrono::seconds{1},
      .optStdoutSink = StreamSink{.path = sinkPath},
    };
    REQUIRE(runner.run(request).exitCode == 0);

    request.argv = {"sh", "-c", "echo second"};
    request.optStdoutSink = StreamSink{.path = sinkPath, .append = true};
    REQUIRE(runner.run(request).exitCode == 0);
    CHECK(ao::test::readFile(sinkPath) == "first\nsecond\n");

    request.argv = {"sh", "-c", "echo third"};
    request.optStdoutSink = StreamSink{.path = sinkPath};
    REQUIRE(runner.run(request).exitCode == 0);
    CHECK(ao::test::readFile(sinkPath) == "third\n");
  }

  TEST_CASE("Process runner - stdout and stderr may share one sink file", "[council][integration][process]")
  {
    auto temp = ao::test::TempDir{};
    auto const sinkPath = temp.path() / "combined.log";
    auto runner = BoostProcessRunner{};
    auto result = runner.run(ProcessRequest{
      .argv = {"sh", "-c", "echo out-line; echo err-line >&2"},
      .cwd = std::filesystem::temp_directory_path(),
      .standardInput = {},
      .environmentWhitelist = {"PATH"},
      .environment = {},
      .timeout = std::chrono::seconds{5},
      .terminationGracePeriod = std::chrono::seconds{1},
      .optStdoutSink = StreamSink{.path = sinkPath},
      .optStderrSink = StreamSink{.path = sinkPath, .append = true},
    });

    CHECK(result.exitCode == 0);
    // Append-mode writes interleave at chunk granularity but never clobber each other.
    auto const log = ao::test::readFile(sinkPath);
    CHECK(log.contains("out-line"));
    CHECK(log.contains("err-line"));
  }

  TEST_CASE("Process runner - onLaunch reports a live pid", "[council][integration][process]")
  {
    auto runner = BoostProcessRunner{};
    std::int64_t launchedPid = -1;
    auto request = ProcessRequest{
      .argv = {"sh", "-c", "exit 0"},
      .cwd = std::filesystem::temp_directory_path(),
      .standardInput = {},
      .environmentWhitelist = {"PATH"},
      .environment = {},
      .timeout = std::chrono::seconds{5},
      .terminationGracePeriod = std::chrono::seconds{1},
    };
    request.onLaunch = [&launchedPid](std::int64_t pid) { launchedPid = pid; };

    auto result = runner.run(request);

    CHECK(result.status == ProcessStatus::Exited);
    CHECK(launchedPid > 0);
  }

  TEST_CASE("Process runner - unopenable sink degrades to in-memory capture", "[council][integration][process]")
  {
    auto runner = BoostProcessRunner{};
    auto result = runner.run(ProcessRequest{
      .argv = {"sh", "-c", "echo survives"},
      .cwd = std::filesystem::temp_directory_path(),
      .standardInput = {},
      .environmentWhitelist = {"PATH"},
      .environment = {},
      .timeout = std::chrono::seconds{5},
      .terminationGracePeriod = std::chrono::seconds{1},
      .optStdoutSink = StreamSink{.path = "/proc/version/impossible/stdout.txt"},
    });

    CHECK(result.status == ProcessStatus::Exited);
    CHECK(result.exitCode == 0);
    CHECK(result.standardOutput == "survives\n");
    CHECK(result.standardError.contains("cannot open sink"));
  }

  TEST_CASE("Process runner - unread standard input neither kills nor hangs the runner",
            "[council][integration][process]")
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
} // namespace ao::council::test
