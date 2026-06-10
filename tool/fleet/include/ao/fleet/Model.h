// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace ao::fleet
{
  namespace
  {
    constexpr auto kDefaultAgentTimeout = std::chrono::minutes{20};
    constexpr auto kDefaultChurnLines = std::size_t{2'000};
  } // namespace

  enum class ScopeOperation : std::uint8_t
  {
    Create,
    Modify,
    Delete,
  };

  enum class EngineKind : std::uint8_t
  {
    Gate,
    Synthesis,
    Search,
  };

  enum class OutputMode : std::uint8_t
  {
    Proposal,
    Advisory,
  };

  enum class FailureReason : std::uint8_t
  {
    None,
    NoCandidate,
    ScopeViolation,
    ChurnExceeded,
    OracleFailed,
    RiskOracleFired,
    BudgetExhausted,
    Infrastructure,
    DependencyFailed,
    RealTreeChanged,
    QuorumFailed,
    RoutePaused,
  };

  enum class FilesystemAuthority : std::uint8_t
  {
    ReadOnly,
    WritableCopy,
    MutateRealTree,
  };

  enum class NetworkAuthority : std::uint8_t
  {
    Off,
    Vendor,
    Full,
  };

  enum class ContextView : std::uint8_t
  {
    Minimal,
    Full,
  };

  enum class PromptDelivery : std::uint8_t
  {
    Stdin,
    Argument,
    File,
  };

  enum class OracleRunner : std::uint8_t
  {
    TestAll,
    TestCore,
    TestGtk,
    TidyClean,
    BuildDebug,
    TestDelta,
    PublicSignatureDelta,
  };

  enum class BaselinePolicy : std::uint8_t
  {
    RequireGreen,
    AllowRed,
    Skip,
  };

  enum class EscalationAction : std::uint8_t
  {
    Retry,
    SwitchRoute,
    RequireCouncil,
    StopRoute,
    ReturnChair,
  };

  enum class CouncilDepth : std::uint8_t
  {
    Panel,
    Challenge,
    Full,
  };

  enum class ReviewVerdict : std::uint8_t
  {
    Accept,
    Modify,
    Reject,
  };

  enum class ProcessStatus : std::uint8_t
  {
    Exited,
    Signaled,
    TimedOut,
    LaunchFailed,
  };

  struct ScopeRule
  {
    std::filesystem::path path;
    std::set<ScopeOperation> operations;

    bool operator==(ScopeRule const&) const = default;
  };

  struct IntentOverrides
  {
    std::optional<std::string> optAgent;
    std::optional<EngineKind> optEngine;
    std::optional<std::string> optOracle;
    std::optional<std::string> optRiskOracle;
    std::optional<std::string> optAuthority;
    std::optional<std::size_t> optFanout;
    std::optional<std::size_t> optTopK;
    std::optional<std::size_t> optMaxRounds;
    std::optional<std::size_t> optChurnLines;
    std::optional<CouncilDepth> optDepth;
    std::optional<std::size_t> optQuorum;
  };

  struct PhaseIntent
  {
    std::string id;
    std::string taskKind;
    std::string invariant;
    std::vector<ScopeRule> scope;
    std::vector<std::string> dependsOn;
    IntentOverrides overrides;
    std::string body;
  };

  struct AgentDefinition
  {
    std::string id;
    std::string vendor;
    std::string model;
    std::vector<std::string> argvTemplate;
    PromptDelivery promptDelivery = PromptDelivery::Stdin;
    std::vector<std::string> environmentWhitelist;
    std::vector<std::filesystem::path> credentialMounts;
    std::chrono::milliseconds timeout{kDefaultAgentTimeout};
    std::string rateLimitKey;
    std::string defaultAuthority;
  };

  struct OracleDefinition
  {
    std::string id;
    OracleRunner runner = OracleRunner::TestAll;
    std::map<std::string, std::string, std::less<>> arguments;
    std::string property;
    std::vector<std::string> prerequisites;
    std::vector<std::string> knownGaps;
    BaselinePolicy baselinePolicy = BaselinePolicy::RequireGreen;
    std::vector<std::filesystem::path> rulerPaths;
  };

  struct AuthorityPolicy
  {
    std::string id;
    FilesystemAuthority filesystem = FilesystemAuthority::ReadOnly;
    NetworkAuthority network = NetworkAuthority::Off;
    ContextView contextView = ContextView::Full;
  };

  struct GateParameters
  {
    std::size_t fanout = 1;
    std::size_t topK = 1;
    std::size_t maxRounds = 1;
    std::size_t churnLines = kDefaultChurnLines;
  };

  struct SynthesisParameters
  {
    std::vector<std::string> roster;
    CouncilDepth depth = CouncilDepth::Challenge;
    std::size_t quorum = 2;
  };

  struct Binding
  {
    std::string taskKind;
    std::string agent;
    EngineKind engine = EngineKind::Gate;
    std::optional<std::string> optOracle;
    std::optional<std::string> optRiskOracle;
    std::string authority;
    GateParameters gate;
    SynthesisParameters synthesis;
  };

  struct EscalationRule
  {
    FailureReason reason = FailureReason::Infrastructure;
    EscalationAction action = EscalationAction::ReturnChair;
    std::optional<std::string> optRoute;
    std::size_t retryLimit = 0;
  };

  struct Registry
  {
    std::map<std::string, AgentDefinition, std::less<>> agents;
    std::map<std::string, OracleDefinition, std::less<>> oracles;
    std::map<std::string, AuthorityPolicy, std::less<>> authorities;
    std::map<std::string, Binding, std::less<>> bindings;
    std::map<FailureReason, EscalationRule> escalations;
  };

  struct ResolvedPhase
  {
    PhaseIntent intent;
    AgentDefinition agent;
    Binding binding;
    AuthorityPolicy authority;
    std::optional<OracleDefinition> optOracle;
    std::optional<OracleDefinition> optRiskOracle;
  };

  struct ProcessResult
  {
    ProcessStatus status = ProcessStatus::LaunchFailed;
    std::int32_t exitCode = -1;
    std::int32_t signal = 0;
    std::string standardOutput;
    std::string standardError;
    std::chrono::milliseconds elapsed{0};
  };

  struct PatchArtifact
  {
    std::string candidateId;
    std::string patch;
    std::vector<std::filesystem::path> touchedFiles;
    std::size_t addedLines = 0;
    std::size_t removedLines = 0;
  };

  struct OracleEvidence
  {
    std::string oracleId;
    std::string oracleVersion;
    std::string property;
    bool passed = false;
    bool infrastructureError = false;
    std::int32_t exitCode = -1;
    std::vector<std::string> knownGaps;
    std::string log;
  };

  struct RiskEvidence
  {
    std::string oracleId;
    bool fired = false;
    std::string detail;
  };

  struct InfrastructureFailure
  {
    std::string phaseId;
    std::string detail;
  };

  struct Escalation
  {
    std::string phaseId;
    FailureReason reason = FailureReason::Infrastructure;
    std::string detail;
  };

  struct RouteKey
  {
    std::string agentId;
    std::string modelVersion;
    std::string harness;
    EngineKind engine = EngineKind::Gate;
    std::string oracleId;
    std::string oracleVersion;
    std::string authority;
    std::string scopeRiskClass;

    std::string canonical() const;
  };

  struct ReviewManifest
  {
    std::string phaseId;
    OutputMode mode = OutputMode::Advisory;
    FailureReason failure = FailureReason::None;
    std::optional<PatchArtifact> optPatch;
    std::vector<OracleEvidence> oracleEvidence;
    std::vector<RiskEvidence> riskEvidence;
    RouteKey route;
    std::string summary;
  };

  AuthorityPolicy intersectAuthority(AuthorityPolicy const& agent,
                                     AuthorityPolicy const& binding,
                                     AuthorityPolicy const& engine);
  std::string makePhaseId();

  std::string_view toString(ScopeOperation value);
  std::string_view toString(EngineKind value);
  std::string_view toString(OutputMode value);
  std::string_view toString(FailureReason value);
  std::string_view toString(FilesystemAuthority value);
  std::string_view toString(NetworkAuthority value);
  std::string_view toString(ContextView value);
  std::string_view toString(OracleRunner value);
  std::string_view toString(BaselinePolicy value);
  std::string_view toString(EscalationAction value);
  std::string_view toString(CouncilDepth value);
  std::string_view toString(ReviewVerdict value);
  std::string_view toString(ProcessStatus value);
} // namespace ao::fleet
