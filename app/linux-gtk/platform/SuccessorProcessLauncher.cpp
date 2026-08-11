// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "platform/SuccessorProcessLauncher.h"

#include "app/GtkStartupPlan.h"
#include <ao/Error.h>
#include <ao/utility/Path.h>

#include <boost/asio/io_context.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/process/v2/default_launcher.hpp>
#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>
#include <boost/system/error_code.hpp>
#include <unistd.h>

#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
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

    template<typename... Initializers>
    Result<> spawnDetached(SuccessorLaunchPlan const& plan, Initializers&&... initializers)
    {
      auto ioContext = boost::asio::io_context{};
      auto launcher = bp::default_process_launcher{};
      auto error = boost::system::error_code{};
      auto const executable = boost::filesystem::path{utility::pathToUtf8(plan.executable)};
      auto process =
        launcher(ioContext, error, executable, plan.arguments, std::forward<Initializers>(initializers)...);

      if (error)
      {
        return makeError(Error::Code::InitFailed,
                         std::format("Failed to launch successor Aobus process '{}': {}",
                                     utility::pathToUtf8(plan.executable),
                                     error.message()));
      }

      std::ignore = process.detach();
      return {};
    }
  } // namespace

  Result<SuccessorLaunchPlan> planSuccessorLaunch(std::filesystem::path const& libraryRoot,
                                                  bool const scanAfterOpen,
                                                  std::optional<std::string_view> const optActivationToken,
                                                  std::optional<std::filesystem::path> const optAppImageExecutable)
  {
    if (libraryRoot.empty() || !libraryRoot.is_absolute())
    {
      return makeError(Error::Code::InvalidInput, "A successor process requires an absolute library root");
    }

    auto plan = SuccessorLaunchPlan{
      .executable = optAppImageExecutable && isUsableAppImage(*optAppImageExecutable)
                      ? *optAppImageExecutable
                      : std::filesystem::path{kProcSelfExecutable},
      .arguments = {std::string{kSuccessorOption},
                    std::string{kLibraryRootOption},
                    utility::pathToUtf8(libraryRoot.lexically_normal())},
      .optActivationToken = std::nullopt,
    };

    if (scanAfterOpen)
    {
      plan.arguments.emplace_back(kScanAfterOpenOption);
    }

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

    auto environmentStorage = makeChildEnvironment(plan.optActivationToken);
    auto processEnvironment = bp::process_environment{environmentStorage};
    return spawnDetached(plan, bp::process_stdio{}, processEnvironment);
  }

  Result<> launchDetachedSuccessor(std::filesystem::path const& libraryRoot,
                                   bool const scanAfterOpen,
                                   std::optional<std::string_view> const optActivationToken)
  {
    auto planRes =
      planSuccessorLaunch(libraryRoot, scanAfterOpen, optActivationToken, appImageExecutableFromEnvironment());

    if (!planRes)
    {
      return std::unexpected{planRes.error()};
    }

    return launchDetachedSuccessor(*planRes);
  }
} // namespace ao::gtk
