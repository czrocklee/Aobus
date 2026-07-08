// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "council/CouncilSchema.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ao::council
{
  inline constexpr auto kDefaultProcessTimeout = std::chrono::minutes{20};
  inline constexpr auto kDefaultTerminationGracePeriod = std::chrono::seconds{2};

  // Live output sink: drained chunks are flushed here as they arrive, so partial output
  // survives a timeout or kill. The in-memory capture in ProcessResult stays authoritative.
  struct StreamSink
  {
    std::filesystem::path path = {};
    bool append = false;
  };

  struct ProcessRequest
  {
    std::vector<std::string> argv = {};
    std::filesystem::path cwd = {};
    std::string standardInput = {};
    std::vector<std::string> environmentWhitelist = {};
    std::map<std::string, std::string, std::less<>> environment = {};
    std::chrono::milliseconds timeout{kDefaultProcessTimeout};
    std::chrono::milliseconds terminationGracePeriod{kDefaultTerminationGracePeriod};
    std::size_t maxCapturedBytes = kDefaultMaxCapturedStreamBytes;
    std::optional<StreamSink> optStdoutSink = std::nullopt;
    std::optional<StreamSink> optStderrSink = std::nullopt;
    // Invoked once on the run() thread right after spawn, before any output is drained.
    std::function<void(std::int64_t pid)> onLaunch = {};
  };

  class ProcessRunner
  {
  public:
    ProcessRunner() = default;
    ProcessRunner(ProcessRunner const&) = delete;
    ProcessRunner& operator=(ProcessRunner const&) = delete;
    ProcessRunner(ProcessRunner&&) = delete;
    ProcessRunner& operator=(ProcessRunner&&) = delete;
    virtual ~ProcessRunner() = default;
    virtual ProcessResult run(ProcessRequest const& request) = 0;
  };

  class BoostProcessRunner final : public ProcessRunner
  {
  public:
    ProcessResult run(ProcessRequest const& request) override;
  };
} // namespace ao::council
