// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/fleet/Model.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ao::fleet
{
  namespace
  {
    constexpr auto kDefaultProcessTimeout = std::chrono::minutes{20};
    constexpr auto kDefaultTerminationGrace = std::chrono::seconds{2};
  } // namespace

  struct ProcessRequest
  {
    std::vector<std::string> argv;
    std::filesystem::path cwd;
    std::string standardInput;
    std::vector<std::string> environmentWhitelist;
    std::map<std::string, std::string, std::less<>> environment;
    std::chrono::milliseconds timeout{kDefaultProcessTimeout};
    std::chrono::milliseconds terminationGrace{kDefaultTerminationGrace};
  };

  class IProcessRunner
  {
  public:
    IProcessRunner() = default;
    IProcessRunner(IProcessRunner const&) = delete;
    IProcessRunner& operator=(IProcessRunner const&) = delete;
    IProcessRunner(IProcessRunner&&) = delete;
    IProcessRunner& operator=(IProcessRunner&&) = delete;
    virtual ~IProcessRunner() = default;
    virtual ProcessResult run(ProcessRequest const& request) = 0;
  };

  class BoostProcessRunner final : public IProcessRunner
  {
  public:
    ProcessResult run(ProcessRequest const& request) override;
  };
} // namespace ao::fleet
