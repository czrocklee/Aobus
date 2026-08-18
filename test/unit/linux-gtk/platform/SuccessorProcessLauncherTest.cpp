// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "platform/SuccessorProcessLauncher.h"

#include "test/unit/TestFixtureSupport.h"
#include <ao/Error.h>
#include <ao/desktop/LibrarySuccessorProtocol.h>
#include <ao/desktop/LibrarySwitch.h>
#include <ao/utility/ScopedRegistration.h>

#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace ao::gtk::test
{
  namespace
  {
    constexpr std::string_view kActivationTokenEnvironment = "XDG_ACTIVATION_TOKEN";
    constexpr std::string_view kPreservedEnvironment = "AOBUS_SUCCESSOR_ENV_PROBE";

    class [[nodiscard]] ScopedEnvironmentValue final
    {
    public:
      ScopedEnvironmentValue(std::string_view const name, std::optional<std::string_view> const optValue)
        : _name{name}
      {
        if (auto const* const value = std::getenv(_name.c_str()); value != nullptr)
        {
          _optPreviousValue = value;
        }

        auto const status =
          optValue ? ::setenv(_name.c_str(), std::string{*optValue}.c_str(), 1) : ::unsetenv(_name.c_str());

        if (status != 0)
        {
          throw std::system_error{errno, std::system_category(), "failed to arrange child environment probe"};
        }
      }

      ~ScopedEnvironmentValue() noexcept
      {
        if (_optPreviousValue)
        {
          std::ignore = ::setenv(_name.c_str(), _optPreviousValue->c_str(), 1);
        }
        else
        {
          std::ignore = ::unsetenv(_name.c_str());
        }
      }

      ScopedEnvironmentValue(ScopedEnvironmentValue const&) = delete;
      ScopedEnvironmentValue& operator=(ScopedEnvironmentValue const&) = delete;
      ScopedEnvironmentValue(ScopedEnvironmentValue&&) = delete;
      ScopedEnvironmentValue& operator=(ScopedEnvironmentValue&&) = delete;

    private:
      std::string _name;
      std::optional<std::string> _optPreviousValue;
    };

    class [[nodiscard]] DetachedChildOutput final
    {
    public:
      DetachedChildOutput()
        : _fifoPath{_tempDir.path() / "environment.fifo"}
      {
        if (::mkfifo(_fifoPath.c_str(), 0600) != 0)
        {
          throw std::system_error{errno, std::system_category(), "failed to create child environment probe FIFO"};
        }

        _descriptor = ::open(_fifoPath.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);

        if (_descriptor < 0)
        {
          throw std::system_error{errno, std::system_category(), "failed to open child environment probe FIFO"};
        }
      }

      ~DetachedChildOutput() noexcept
      {
        if (_descriptor >= 0)
        {
          std::ignore = ::close(_descriptor);
        }
      }

      DetachedChildOutput(DetachedChildOutput const&) = delete;
      DetachedChildOutput& operator=(DetachedChildOutput const&) = delete;
      DetachedChildOutput(DetachedChildOutput&&) = delete;
      DetachedChildOutput& operator=(DetachedChildOutput&&) = delete;

      std::filesystem::path const& fifoPath() const { return _fifoPath; }

      /**
       * @brief The child's output, once @p expectedLines lines have arrived.
       *
       * A line count rather than a read to end of stream, because this holds the
       * FIFO open for writing to keep the path valid before the child starts:
       * there is always a writer, so the reader is never handed the end and has
       * to be told how much to wait for.
       *
       * Waiting matters. The probe is a shell `printf` with one conversion per
       * line, which reaches the FIFO as one write per line, so a single read
       * returns whichever lines had landed by then — usually all of them, and
       * under load the first one alone. That made a correct child look like a
       * child that dropped its environment.
       */
      std::string read(std::size_t const expectedLines)
      {
        auto output = std::string{};
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};

        while (std::cmp_less(std::ranges::count(output, '\n'), expectedLines))
        {
          auto const remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());

          if (remaining.count() <= 0)
          {
            throw std::runtime_error{"child environment probe timed out"};
          }

          auto descriptor = ::pollfd{.fd = _descriptor, .events = POLLIN, .revents = 0};
          std::int32_t pollStatus = 0;

          for (;;)
          {
            pollStatus = ::poll(&descriptor, 1, static_cast<std::int32_t>(remaining.count()));

            if (pollStatus >= 0 || errno != EINTR)
            {
              break;
            }
          }

          if (pollStatus == 0)
          {
            throw std::runtime_error{"child environment probe timed out"};
          }

          if (pollStatus < 0)
          {
            throw std::system_error{errno, std::system_category(), "failed to poll child environment probe"};
          }

          if ((descriptor.revents & POLLIN) == 0)
          {
            throw std::runtime_error{"child environment probe produced no readable output"};
          }

          auto buffer = std::array<char, 256>{};
          ssize_t readSize = 0;

          for (;;)
          {
            readSize = ::read(_descriptor, buffer.data(), buffer.size());

            if (readSize >= 0 || errno != EINTR)
            {
              break;
            }
          }

          if (readSize < 0)
          {
            throw std::system_error{errno, std::system_category(), "failed to read child environment probe"};
          }

          output.append(buffer.data(), static_cast<std::size_t>(readSize));
        }

        return output;
      }

    private:
      ao::test::TempDir _tempDir;
      std::filesystem::path _fifoPath;
      int _descriptor = -1;
    };

    SuccessorLaunchPlan environmentProbePlan(std::filesystem::path const& outputPath,
                                             std::optional<std::string> optActivationToken)
    {
      constexpr auto kScript =
        R"(if [ "${XDG_ACTIVATION_TOKEN+x}" = x ]; then token="set:${XDG_ACTIVATION_TOKEN}"; else token=unset; fi; printf '%s\n%s\n' "$token" "${AOBUS_SUCCESSOR_ENV_PROBE-}" > "$1")";
      return SuccessorLaunchPlan{
        .executable = "/bin/sh",
        .arguments = {"-c", kScript, "aobus-successor-environment-probe", outputPath.string()},
        .optActivationToken = std::move(optActivationToken),
      };
    }
  } // namespace

  TEST_CASE("SuccessorProcessLauncher - launch plan prefers an executable APPIMAGE and preserves restart intent",
            "[gtk][unit][process]")
  {
    auto const appImage = ao::test::TempFile{".AppImage"};
    std::filesystem::permissions(appImage.path, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);

    auto result =
      planSuccessorLaunch(desktop::LibrarySwitchRequest{.libraryRoot = "/music/../library", .scanAfterOpen = true},
                          std::string_view{"activation-token"},
                          appImage.path);

    REQUIRE(result);
    CHECK(result->executable == appImage.path);
    CHECK(result->arguments == std::vector<std::string>{std::string{desktop::kLibrarySuccessorOption},
                                                        std::string{desktop::kLibraryRootOption},
                                                        "/library",
                                                        std::string{desktop::kScanAfterOpenOption}});
    CHECK(result->optActivationToken == std::optional<std::string>{"activation-token"});
  }

  TEST_CASE("SuccessorProcessLauncher - launch plan falls back to proc and omits empty optional state",
            "[gtk][unit][process]")
  {
    auto const nonExecutableAppImage = ao::test::TempFile{".AppImage"};

    auto result = planSuccessorLaunch(
      desktop::LibrarySwitchRequest{.libraryRoot = "/music"}, std::string_view{}, nonExecutableAppImage.path);

    REQUIRE(result);
    CHECK(result->executable == std::filesystem::path{"/proc/self/exe"});
    CHECK(result->arguments == std::vector<std::string>{std::string{desktop::kLibrarySuccessorOption},
                                                        std::string{desktop::kLibraryRootOption},
                                                        "/music"});
    CHECK_FALSE(result->optActivationToken);
  }

  TEST_CASE("SuccessorProcessLauncher - relative library root cannot produce a launch plan", "[gtk][unit][process]")
  {
    auto result = planSuccessorLaunch(desktop::LibrarySwitchRequest{.libraryRoot = "music"});

    REQUIRE_FALSE(result);
    CHECK(result.error().code == Error::Code::InvalidInput);
    CHECK(result.error().message.contains("absolute"));
  }

  TEST_CASE("SuccessorProcessLauncher - process creation reports exec failure and detaches success",
            "[gtk][integration][process]")
  {
    SECTION("exec failure")
    {
      auto const plan = SuccessorLaunchPlan{
        .executable = "/aobus-test/nonexistent-successor",
        .arguments = {},
        .optActivationToken = std::nullopt,
      };

      auto result = launchDetachedSuccessor(plan);

      REQUIRE_FALSE(result);
      CHECK(result.error().code == Error::Code::InitFailed);
      CHECK(result.error().message.contains("Failed to launch successor"));
    }

    SECTION("successful process creation")
    {
      auto const tempDir = ao::test::TempDir{};
      auto const gatePath = tempDir.path() / "gate.fifo";
      auto const completionPath = tempDir.path() / "completion.fifo";
      REQUIRE(::mkfifo(gatePath.c_str(), 0600) == 0);
      REQUIRE(::mkfifo(completionPath.c_str(), 0600) == 0);

      auto const gateDescriptor = ::open(gatePath.c_str(), O_RDWR | O_CLOEXEC);
      REQUIRE(gateDescriptor >= 0);
      [[maybe_unused]] auto gateRegistration =
        utility::ScopedRegistration{[gateDescriptor] { std::ignore = ::close(gateDescriptor); }};

      auto const completionDescriptor = ::open(completionPath.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
      REQUIRE(completionDescriptor >= 0);
      [[maybe_unused]] auto completionRegistration =
        utility::ScopedRegistration{[completionDescriptor] { std::ignore = ::close(completionDescriptor); }};

      auto const plan = SuccessorLaunchPlan{
        .executable = "/bin/sh",
        .arguments = {"-c",
                      R"(dd if="$1" of=/dev/null bs=1 count=1 2>/dev/null && printf x > "$2")",
                      "aobus-successor-probe",
                      gatePath.string(),
                      completionPath.string()},
        .optActivationToken = std::nullopt,
      };

      REQUIRE(launchDetachedSuccessor(plan));

      auto const gateByte = std::array{'g'};
      REQUIRE(::write(gateDescriptor, gateByte.data(), gateByte.size()) == 1);

      auto descriptor = ::pollfd{.fd = completionDescriptor, .events = POLLIN, .revents = 0};
      REQUIRE(::poll(&descriptor, 1, 5'000) == 1);
      CHECK((descriptor.revents & POLLIN) != 0);

      auto completionByte = std::array<char, 1>{};
      REQUIRE(::read(completionDescriptor, completionByte.data(), completionByte.size()) == 1);
      CHECK(completionByte.front() == 'x');
    }
  }

  TEST_CASE("SuccessorProcessLauncher - child environment replaces or clears inherited activation token",
            "[gtk][integration][process]")
  {
    [[maybe_unused]] auto const inheritedToken =
      ScopedEnvironmentValue{kActivationTokenEnvironment, std::string_view{"inherited-token"}};
    [[maybe_unused]] auto const preservedValue =
      ScopedEnvironmentValue{kPreservedEnvironment, std::string_view{"preserved-value"}};

    SECTION("new activation token replaces inherited value")
    {
      auto output = DetachedChildOutput{};
      auto const plan = environmentProbePlan(output.fifoPath(), std::string{"replacement-token"});

      REQUIRE(launchDetachedSuccessor(plan));

      CHECK(output.read(2) == "set:replacement-token\npreserved-value\n");
    }

    SECTION("missing activation token removes inherited value")
    {
      auto output = DetachedChildOutput{};
      auto const plan = environmentProbePlan(output.fifoPath(), std::nullopt);

      REQUIRE(launchDetachedSuccessor(plan));

      CHECK(output.read(2) == "unset\npreserved-value\n");
    }
  }
} // namespace ao::gtk::test
