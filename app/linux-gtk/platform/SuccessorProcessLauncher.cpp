// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "platform/SuccessorProcessLauncher.h"

#include <ao/Error.h>
#include <ao/desktop/DetachedProcessLauncher.h>
#include <ao/desktop/LibrarySuccessorProtocol.h>
#include <ao/desktop/LibrarySwitch.h>
#include <ao/utility/Path.h>

#include <boost/process/v2/environment.hpp>
#include <unistd.h>

#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ao::gtk
{
  namespace
  {
    namespace bp = boost::process::v2;

    constexpr auto kActivationTokenEnvironment = "XDG_ACTIVATION_TOKEN";
    constexpr auto kAppImageEnvironment = "APPIMAGE";
    constexpr auto kProcSelfExecutable = "/proc/self/exe";

    bool isUsableAppImage(std::filesystem::path const& executable)
    {
      if (!executable.is_absolute())
      {
        return false;
      }

      if (auto error = std::error_code{}; !std::filesystem::is_regular_file(executable, error) || error)
      {
        return false;
      }

      return ::access(executable.c_str(), X_OK) == 0;
    }

    std::optional<std::filesystem::path> appImageExecutableFromEnvironment()
    {
      auto const* const value = std::getenv(kAppImageEnvironment);

      if (value == nullptr || value[0] == '\0')
      {
        return std::nullopt;
      }

      return utility::pathFromUtf8(value);
    }

    std::vector<std::string> makeChildEnvironment(std::optional<std::string> const& optActivationToken)
    {
      auto environment = std::vector<std::string>{};

      for (auto const entry : bp::environment::current())
      {
        if (entry.key().compare(kActivationTokenEnvironment) != 0)
        {
          environment.emplace_back(entry.native().data(), entry.native().size());
        }
      }

      if (optActivationToken && !optActivationToken->empty())
      {
        environment.push_back(std::format("{}={}", kActivationTokenEnvironment, *optActivationToken));
      }

      return environment;
    }
  } // namespace

  Result<SuccessorLaunchPlan> planSuccessorLaunch(desktop::LibrarySwitchRequest const& request,
                                                  std::optional<std::string_view> const optActivationToken,
                                                  std::optional<std::filesystem::path> const optAppImageExecutable)
  {
    auto argumentsRes = desktop::librarySuccessorArguments(request);

    if (!argumentsRes)
    {
      return std::unexpected{argumentsRes.error()};
    }

    auto plan = SuccessorLaunchPlan{
      .executable = optAppImageExecutable && isUsableAppImage(*optAppImageExecutable)
                      ? *optAppImageExecutable
                      : std::filesystem::path{kProcSelfExecutable},
      .arguments = std::move(*argumentsRes),
      .optActivationToken = std::nullopt,
    };

    if (optActivationToken && !optActivationToken->empty())
    {
      plan.optActivationToken = *optActivationToken;
    }

    return plan;
  }

  Result<> launchDetachedSuccessor(SuccessorLaunchPlan const& plan)
  {
    if (plan.executable.empty())
    {
      return makeError(Error::Code::InvalidInput, "A successor launch plan requires an executable");
    }

    auto launchedRes = desktop::launchDetachedProcess({
      .executable = plan.executable,
      .arguments = plan.arguments,
      .optEnvironment = makeChildEnvironment(plan.optActivationToken),
      .standardStreams = desktop::DetachedProcessStandardStreams::InheritParent,
    });

    if (!launchedRes)
    {
      return makeError(launchedRes.error().code,
                       std::format("Failed to launch successor Aobus process: {}", launchedRes.error().message));
    }

    return {};
  }

  Result<> launchDetachedSuccessor(desktop::LibrarySwitchRequest const& request,
                                   std::optional<std::string_view> const optActivationToken)
  {
    auto planRes = planSuccessorLaunch(request, optActivationToken, appImageExecutableFromEnvironment());

    if (!planRes)
    {
      return std::unexpected{planRes.error()};
    }

    return launchDetachedSuccessor(*planRes);
  }
} // namespace ao::gtk
