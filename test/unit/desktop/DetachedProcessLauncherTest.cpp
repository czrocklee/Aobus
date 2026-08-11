// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/desktop/DetachedProcessLauncher.h>

#include <ao/Error.h>

#include <catch2/catch_test_macros.hpp>

#ifndef _WIN32
#include "test/unit/TestFixtureSupport.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <filesystem>
#include <sys/stat.h>
#include <system_error>
#include <tuple>
#endif

namespace ao::desktop::test
{
  TEST_CASE("DetachedProcessLauncher - invalid executable reports process creation failure",
            "[runtime][integration][desktop-process]")
  {
    auto result = launchDetachedProcess({.executable = "/aobus-test/nonexistent-successor", .arguments = {}});

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::InitFailed);
    CHECK(result.error().message.contains("Failed to launch detached process"));
  }

#ifndef _WIN32

  TEST_CASE("DetachedProcessLauncher - detached child completes after launcher ownership ends",
            "[runtime][integration][desktop-process]")
  {
    auto const fixture = ao::test::TempDir{};
    auto const completionPath = fixture.path() / "completion.fifo";
    REQUIRE(::mkfifo(completionPath.c_str(), 0600) == 0);

    auto const descriptor = ::open(completionPath.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);

    if (descriptor < 0)
    {
      throw std::system_error{errno, std::system_category(), "failed to open detached child completion FIFO"};
    }

    auto const launch = DetachedProcessLaunch{
      .executable = "/bin/sh",
      .arguments = {"-c", R"(printf x > "$1")", "aobus-detached-process-probe", completionPath.string()},
      .standardStreams = DetachedProcessStandardStreams::InheritParent,
    };
    REQUIRE(launchDetachedProcess(launch));

    auto pollDescriptor = ::pollfd{.fd = descriptor, .events = POLLIN, .revents = 0};
    auto const pollResult = ::poll(&pollDescriptor, 1, 5'000);
    auto value = std::array<char, 1>{};
    auto const readResult = pollResult == 1 ? ::read(descriptor, value.data(), value.size()) : -1;
    std::ignore = ::close(descriptor);

    REQUIRE(pollResult == 1);
    REQUIRE(readResult == 1);
    CHECK(value.front() == 'x');
  }
#endif
} // namespace ao::desktop::test
