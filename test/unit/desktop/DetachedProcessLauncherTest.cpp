// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/desktop/DetachedProcessLauncher.h>

#include <ao/Error.h>

#include <catch2/catch_test_macros.hpp>

#ifndef _WIN32
#include "test/unit/TestFixtureSupport.h"
#include <ao/utility/ScopedRegistration.h>

#include <fcntl.h>
#include <gsl-lite/gsl-lite.hpp>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <future>
#include <sys/poll.h>
#include <sys/stat.h>
#include <tuple>
#endif

namespace ao::desktop::test
{
  TEST_CASE("DetachedProcessLauncher - invalid executable reports process creation failure",
            "[runtime][integration][desktop-process]")
  {
    auto res = launchDetachedProcess({.executable = "/aobus-test/nonexistent-successor", .arguments = {}});

    REQUIRE_FALSE(res);
    CHECK(res.error().code == Error::Code::InitFailed);
    CHECK(res.error().message.contains("Failed to launch detached process"));
  }

#ifndef _WIN32

  TEST_CASE("DetachedProcessLauncher - detached child completes after launcher ownership ends",
            "[runtime][integration][desktop-process]")
  {
    auto const fixture = ao::test::TempDir{};
    auto const gatePath = fixture.path() / "gate.fifo";
    auto const completionPath = fixture.path() / "completion.fifo";
    REQUIRE(::mkfifo(gatePath.c_str(), 0600) == 0);
    REQUIRE(::mkfifo(completionPath.c_str(), 0600) == 0);

    auto const gateDescriptor = ::open(gatePath.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
    REQUIRE(gateDescriptor >= 0);
    [[maybe_unused]] auto gateRegistration =
      gsl_lite::finally([gateDescriptor] { std::ignore = ::close(gateDescriptor); });

    auto const descriptor = ::open(completionPath.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
    REQUIRE(descriptor >= 0);
    [[maybe_unused]] auto completionRegistration =
      gsl_lite::finally([descriptor] { std::ignore = ::close(descriptor); });

    auto const launch = DetachedProcessLaunch{
      .executable = "/bin/sh",
      .arguments = {"-c",
                    R"(dd if="$1" of=/dev/null bs=1 count=1 2>/dev/null && printf x > "$2")",
                    "aobus-detached-process-probe",
                    gatePath.string(),
                    completionPath.string()},
      .standardStreams = DetachedProcessStandardStreams::InheritParent,
    };
    auto launchTask = std::future<Result<>>{};
    decltype(::write(gateDescriptor, nullptr, 0)) gateWriteSize = -1;
    auto releaseChild = utility::ScopedRegistration{[&] noexcept
                                                    {
                                                      auto const gateByte = std::array{'g'};

                                                      for (;;)
                                                      {
                                                        gateWriteSize =
                                                          ::write(gateDescriptor, gateByte.data(), gateByte.size());

                                                        if (gateWriteSize >= 0 || errno != EINTR)
                                                        {
                                                          break;
                                                        }
                                                      }
                                                    }};
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    launchTask = std::async(std::launch::async, [&launch] { return launchDetachedProcess(launch); });
    auto const returnedBeforeRelease =
      launchTask.wait_for(deadline - std::chrono::steady_clock::now()) == std::future_status::ready;
    CHECK(returnedBeforeRelease);

    // Release before any failing assertion can destroy the future and wait for the launcher.
    releaseChild.reset();
    REQUIRE(gateWriteSize == 1);
    REQUIRE(launchTask.get());

    auto completionByte = std::array<char, 1>{};

    for (;;)
    {
      auto const remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      REQUIRE(remaining.count() > 0);
      auto pollDescriptor = ::pollfd{.fd = descriptor, .events = POLLIN, .revents = 0};
      auto const pollResult = ::poll(&pollDescriptor, 1, static_cast<std::int32_t>(remaining.count()));

      if (pollResult < 0 && errno == EINTR)
      {
        continue;
      }

      REQUIRE(pollResult == 1);
      REQUIRE((pollDescriptor.revents & POLLIN) != 0);
      auto const readResult = ::read(descriptor, completionByte.data(), completionByte.size());

      if (readResult < 0 && (errno == EINTR || errno == EAGAIN))
      {
        continue;
      }

      REQUIRE(readResult == 1);
      break;
    }

    CHECK(completionByte.front() == 'x');
  }
#endif
} // namespace ao::desktop::test
