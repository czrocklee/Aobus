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
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::fleet
{
  inline constexpr auto kDefaultAgentTimeout = std::chrono::minutes{20};
  inline constexpr auto kDefaultChurnLines = std::size_t{2'000};

  // Every enum below pairs with a kXxxNames table: the single source of truth shared by
  // toString and the YAML parsers, so a new enumerator is wired up in exactly one place.
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

  enum class ScopeOperation : std::uint8_t
  {
    Create,
    Modify,
    Delete,
  };

  inline constexpr auto kScopeOperationNames = EnumNameTable<ScopeOperation, 3>{{
    {ScopeOperation::Create, "create"},
    {ScopeOperation::Modify, "modify"},
    {ScopeOperation::Delete, "delete"},
  }};

  enum class EngineKind : std::uint8_t
  {
    Gate,
    Synthesis,
    Search,
  };

  inline constexpr auto kEngineKindNames = EnumNameTable<EngineKind, 3>{{
    {EngineKind::Gate, "gate"},
    {EngineKind::Synthesis, "synthesis"},
    {EngineKind::Search, "search"},
  }};

  enum class OutputMode : std::uint8_t
  {
    Proposal,
    Advisory,
  };

  inline constexpr auto kOutputModeNames = EnumNameTable<OutputMode, 2>{{
    {OutputMode::Proposal, "proposal"},
    {OutputMode::Advisory, "advisory"},
  }};

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

  inline constexpr auto kFailureReasonNames = EnumNameTable<FailureReason, 12>{{
    {FailureReason::None, "none"},
    {FailureReason::NoCandidate, "no-candidate"},
    {FailureReason::ScopeViolation, "scope-violation"},
    {FailureReason::ChurnExceeded, "churn-exceeded"},
    {FailureReason::OracleFailed, "oracle-failed"},
    {FailureReason::RiskOracleFired, "risk-oracle-fired"},
    {FailureReason::BudgetExhausted, "budget-exhausted"},
    {FailureReason::Infrastructure, "infrastructure"},
    {FailureReason::DependencyFailed, "dependency-failed"},
    {FailureReason::RealTreeChanged, "real-tree-changed"},
    {FailureReason::QuorumFailed, "quorum-failed"},
    {FailureReason::RoutePaused, "route-paused"},
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

  enum class OracleRunner : std::uint8_t
  {
    TestAll,
    TestCore,
    TestGtk,
    TestAsan,
    TestTsan,
    TidyClean,
    BuildDebug,
    TestDelta,
    PublicSignatureDelta,
  };

  inline constexpr auto kOracleRunnerNames = EnumNameTable<OracleRunner, 9>{{
    {OracleRunner::TestAll, "test-all"},
    {OracleRunner::TestCore, "test-core"},
    {OracleRunner::TestGtk, "test-gtk"},
    {OracleRunner::TestAsan, "test-asan"},
    {OracleRunner::TestTsan, "test-tsan"},
    {OracleRunner::TidyClean, "tidy-clean"},
    {OracleRunner::BuildDebug, "build-debug"},
    {OracleRunner::TestDelta, "test-delta"},
    {OracleRunner::PublicSignatureDelta, "public-signature-delta"},
  }};

  enum class BaselinePolicy : std::uint8_t
  {
    RequireGreen,
    AllowRed,
    Skip,
  };

  inline constexpr auto kBaselinePolicyNames = EnumNameTable<BaselinePolicy, 3>{{
    {BaselinePolicy::RequireGreen, "require-green"},
    {BaselinePolicy::AllowRed, "allow-red"},
    {BaselinePolicy::Skip, "skip"},
  }};

  enum class EscalationAction : std::uint8_t
  {
    Retry,
    SwitchRoute,
    RequireCouncil,
    StopRoute,
    ReturnChair,
  };

  inline constexpr auto kEscalationActionNames = EnumNameTable<EscalationAction, 5>{{
    {EscalationAction::Retry, "retry"},
    {EscalationAction::SwitchRoute, "switch-route"},
    {EscalationAction::RequireCouncil, "require-council"},
    {EscalationAction::StopRoute, "stop-route"},
    {EscalationAction::ReturnChair, "return-chair"},
  }};

  enum class CouncilDepth : std::uint8_t
  {
    Panel,
    Challenge,
    Full,
  };

  inline constexpr auto kCouncilDepthNames = EnumNameTable<CouncilDepth, 3>{{
    {CouncilDepth::Panel, "panel"},
    {CouncilDepth::Challenge, "challenge"},
    {CouncilDepth::Full, "full"},
  }};

  enum class ReviewVerdict : std::uint8_t
  {
    Accept,
    Modify,
    Reject,
  };

  inline constexpr auto kReviewVerdictNames = EnumNameTable<ReviewVerdict, 3>{{
    {ReviewVerdict::Accept, "accept"},
    {ReviewVerdict::Modify, "modify"},
    {ReviewVerdict::Reject, "reject"},
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
    std::optional<std::size_t> optFanout;
    std::optional<std::size_t> optTopK;
    std::optional<std::size_t> optMaxRounds;
    std::optional<std::size_t> optChurnLines;
    std::optional<CouncilDepth> optDepth;
    std::optional<std::size_t> optQuorum;

    bool operator==(IntentOverrides const&) const = default;
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

  // A harness is the headless CLI invocation shared by every agent that runs through it:
  // argv template (with the static {model}/{effort} and the run-time prompt placeholders),
  // prompt delivery, environment, default timeout, and the quota pool it draws from.
  struct HarnessDefinition
  {
    std::string id;
    std::vector<std::string> argvTemplate;
    PromptDelivery promptDelivery = PromptDelivery::Stdin;
    std::vector<std::string> environmentWhitelist;
    std::chrono::milliseconds timeout{kDefaultAgentTimeout};
    std::string rateLimitKey;
  };

  // An agent is a fleet member identity: (harness, model) plus the vendor whose weights answer
  // and an optional reasoning-effort knob. The invocation fields are resolved from the harness
  // at registry load, with {model}/{effort} already substituted into the argv template.
  struct AgentDefinition
  {
    std::string id;
    std::string harness;
    std::string model;
    std::string vendor;
    std::string effort;
    std::vector<std::string> argvTemplate;
    PromptDelivery promptDelivery = PromptDelivery::Stdin;
    std::vector<std::string> environmentWhitelist;
    std::chrono::milliseconds timeout{kDefaultAgentTimeout};
    std::string rateLimitKey;

    // Route identity: the same model at a different effort level is a different actor, so the
    // effort folds into the version the breaker and failure manifests key on.
    std::string modelVersion() const { return effort.empty() ? model : model + "@" + effort; }
  };

  struct OracleDefinition
  {
    std::string id;
    OracleRunner runner = OracleRunner::TestAll;
    std::map<std::string, std::string, std::less<>> arguments;
    std::string property;
    std::vector<std::string> knownGaps;
    BaselinePolicy baselinePolicy = BaselinePolicy::RequireGreen;
    std::vector<std::filesystem::path> rulerPaths;
    std::optional<std::chrono::milliseconds> optTimeout;
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
    GateParameters gate;
    SynthesisParameters synthesis;
  };

  // How an intent override merges into the binding it targets.
  enum class OverridePolicy : std::uint8_t
  {
    Assign,       // replaces the binding value outright
    TightenUpper, // may only lower the binding value (budget-style caps)
    TightenLower, // may only raise the binding value (quorum-style floors)
  };

  // Single source of truth for the intent override fields: yaml key, storage member,
  // merge policy, and the binding slot the override applies to. Parsing, allowed-key
  // validation, presence checks, emission, and application all iterate this list, so
  // adding an override field is a one-entry change.
  template<typename Visitor>
  void forEachOverrideField(Visitor const& visit)
  {
    visit("agent", &IntentOverrides::optAgent, OverridePolicy::Assign, &Binding::agent);
    visit("engine", &IntentOverrides::optEngine, OverridePolicy::Assign, &Binding::engine);
    visit("oracle", &IntentOverrides::optOracle, OverridePolicy::Assign, &Binding::optOracle);
    visit("risk-oracle", &IntentOverrides::optRiskOracle, OverridePolicy::Assign, &Binding::optRiskOracle);
    visit("fanout",
          &IntentOverrides::optFanout,
          OverridePolicy::TightenUpper,
          [](Binding& binding) -> auto& { return binding.gate.fanout; });
    visit("top-k",
          &IntentOverrides::optTopK,
          OverridePolicy::TightenUpper,
          [](Binding& binding) -> auto& { return binding.gate.topK; });
    visit("max-rounds",
          &IntentOverrides::optMaxRounds,
          OverridePolicy::TightenUpper,
          [](Binding& binding) -> auto& { return binding.gate.maxRounds; });
    visit("churn-lines",
          &IntentOverrides::optChurnLines,
          OverridePolicy::TightenUpper,
          [](Binding& binding) -> auto& { return binding.gate.churnLines; });
    visit("depth",
          &IntentOverrides::optDepth,
          OverridePolicy::TightenUpper,
          [](Binding& binding) -> auto& { return binding.synthesis.depth; });
    visit("quorum",
          &IntentOverrides::optQuorum,
          OverridePolicy::TightenLower,
          [](Binding& binding) -> auto& { return binding.synthesis.quorum; });
  }

  struct EscalationRule
  {
    FailureReason reason = FailureReason::Infrastructure;
    EscalationAction action = EscalationAction::ReturnChair;
    std::optional<std::string> optRoute;
    std::size_t retryLimit = 0;
  };

  struct Registry
  {
    std::map<std::string, HarnessDefinition, std::less<>> harnesses;
    std::map<std::string, AgentDefinition, std::less<>> agents;
    std::map<std::string, OracleDefinition, std::less<>> oracles;
    std::map<std::string, Binding, std::less<>> bindings;
    std::map<FailureReason, EscalationRule> escalations;
    // Paths every patch guard enforces in addition to the per-oracle rulers and the
    // non-negotiable self-protection core hardcoded in the engine.
    std::vector<std::filesystem::path> rulerPaths;
  };

  struct ResolvedPhase
  {
    PhaseIntent intent;
    AgentDefinition agent;
    Binding binding;
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

  struct TouchedFile
  {
    std::filesystem::path path;
    ScopeOperation operation = ScopeOperation::Modify;

    bool operator==(TouchedFile const&) const = default;
  };

  struct PatchArtifact
  {
    std::string candidateId;
    std::string patch;
    std::vector<TouchedFile> touchedFiles;
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
    // The registry escalation action for the failure reason, so the chair can act on the
    // manifest without re-deriving policy; empty when the phase did not fail.
    std::optional<EscalationAction> optEscalationAction;
  };

  std::string makePhaseId();

  std::string_view toString(ScopeOperation value);
  std::string_view toString(EngineKind value);
  std::string_view toString(OutputMode value);
  std::string_view toString(FailureReason value);
  std::string_view toString(PromptDelivery value);
  std::string_view toString(OracleRunner value);
  std::string_view toString(BaselinePolicy value);
  std::string_view toString(EscalationAction value);
  std::string_view toString(CouncilDepth value);
  std::string_view toString(ReviewVerdict value);
  std::string_view toString(ProcessStatus value);
} // namespace ao::fleet
