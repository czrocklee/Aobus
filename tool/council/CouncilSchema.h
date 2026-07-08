// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::council
{
  inline constexpr auto kDefaultAgentTimeout = std::chrono::minutes{20};
  inline constexpr auto kDefaultMaxCapturedStreamBytes = std::size_t{4UL * 1024UL * 1024UL};
  inline constexpr auto kReviewBaseRef = std::string_view{"refs/aobus-council/base"};

  template<typename Enum, std::size_t N>
  using EnumNameTable = std::array<std::pair<Enum, std::string_view>, N>;

  template<typename Enum, std::size_t N>
  constexpr std::string_view enumName(EnumNameTable<Enum, N> const& names, Enum value)
  {
    for (auto const& [item, name] : names)
    {
      if (item == value)
      {
        return name;
      }
    }

    return "invalid";
  }

  template<typename Enum, std::size_t N>
  constexpr std::optional<Enum> enumFromName(EnumNameTable<Enum, N> const& names, std::string_view name)
  {
    for (auto const& [item, itemName] : names)
    {
      if (itemName == name)
      {
        return item;
      }
    }

    return std::nullopt;
  }

  enum class FocusMatch : std::uint8_t
  {
    Exact,
    Prefix,
  };

  enum class FailureReason : std::uint8_t
  {
    None,
    DependencyFailed,
    QuorumFailed,
    InfrastructureFailed,
    RealTreeChanged,
  };

  inline constexpr auto kFailureReasonNames = EnumNameTable<FailureReason, 5>{{
    {FailureReason::None, "none"},
    {FailureReason::DependencyFailed, "dependency-failed"},
    {FailureReason::QuorumFailed, "quorum-failed"},
    {FailureReason::InfrastructureFailed, "infrastructure-failed"},
    {FailureReason::RealTreeChanged, "real-tree-changed"},
  }};

  enum class PromptDelivery : std::uint8_t
  {
    Stdin,
    Argument,
    File,
  };

  inline constexpr auto kPromptDeliveryNames = EnumNameTable<PromptDelivery, 3>{{
    {PromptDelivery::Stdin, "stdin"},
    {PromptDelivery::Argument, "argument"},
    {PromptDelivery::File, "file"},
  }};

  enum class Depth : std::uint8_t
  {
    Panel,
    Challenge,
    Full,
  };

  inline constexpr auto kDepthNames = EnumNameTable<Depth, 3>{{
    {Depth::Panel, "panel"},
    {Depth::Challenge, "challenge"},
    {Depth::Full, "full"},
  }};

  enum class ProcessStatus : std::uint8_t
  {
    Exited,
    Signaled,
    TimedOut,
    LaunchFailed,
  };

  inline constexpr auto kProcessStatusNames = EnumNameTable<ProcessStatus, 4>{{
    {ProcessStatus::Exited, "exited"},
    {ProcessStatus::Signaled, "signaled"},
    {ProcessStatus::TimedOut, "timed-out"},
    {ProcessStatus::LaunchFailed, "launch-failed"},
  }};

  struct FocusRule
  {
    std::filesystem::path path = {};
    FocusMatch match = FocusMatch::Exact;

    bool operator==(FocusRule const&) const = default;
  };

  struct IntentOverrides
  {
    std::optional<std::vector<std::string>> optRoster = std::nullopt;
    std::optional<Depth> optDepth = std::nullopt;
    std::optional<std::size_t> optQuorum = std::nullopt;

    bool operator==(IntentOverrides const&) const = default;
  };

  struct PhaseIntent
  {
    std::string id = {};
    std::string taskKind = {};
    std::string invariant = {};
    std::vector<FocusRule> focus = {};
    std::vector<std::string> dependsOn = {};
    IntentOverrides overrides = {};
    std::string body = {};
  };

  struct HarnessDefinition
  {
    std::string id = {};
    std::vector<std::string> argvTemplate = {};
    PromptDelivery promptDelivery = PromptDelivery::Stdin;
    std::vector<std::string> environmentWhitelist = {};
    std::chrono::milliseconds timeout{kDefaultAgentTimeout};
  };

  struct AgentDefinition
  {
    std::string id = {};
    std::string harness = {};
    std::string model = {};
    std::string vendor = {};
    std::string effort = {};
    std::vector<std::string> argvTemplate = {};
    PromptDelivery promptDelivery = PromptDelivery::Stdin;
    std::vector<std::string> environmentWhitelist = {};
    std::chrono::milliseconds timeout{kDefaultAgentTimeout};

    std::string modelVersion() const { return effort.empty() ? model : model + "@" + effort; }
  };

  struct Parameters
  {
    std::vector<std::string> roster = {};
    Depth depth = Depth::Challenge;
    std::size_t quorum = 2;
  };

  struct Definition
  {
    std::string taskKind = {};
    Parameters parameters = {};
  };

  struct Registry
  {
    std::map<std::string, HarnessDefinition, std::less<>> harnesses = {};
    std::map<std::string, AgentDefinition, std::less<>> agents = {};
    std::map<std::string, Definition, std::less<>> councils = {};
  };

  struct ResolvedPhase
  {
    PhaseIntent intent = {};
    Definition definition = {};
  };

  struct ProcessResult
  {
    ProcessStatus status = ProcessStatus::LaunchFailed;
    std::int32_t exitCode = -1;
    std::int32_t signal = 0;
    std::string standardOutput = {};
    std::string standardError = {};
    bool standardOutputTruncated = false;
    bool standardErrorTruncated = false;
    std::chrono::milliseconds elapsed{0};
  };

  struct ReviewManifest
  {
    std::string phaseId = {};
    FailureReason failure = FailureReason::None;
    std::string summary = {};
  };

  std::string makePhaseId();

  std::string_view toString(FailureReason value);
  std::string_view toString(PromptDelivery value);
  std::string_view toString(Depth value);
  std::string_view toString(ProcessStatus value);
} // namespace ao::council
