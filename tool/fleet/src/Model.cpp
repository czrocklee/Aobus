// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/fleet/Model.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <string>
#include <string_view>

namespace ao::fleet
{
  namespace
  {
    template<typename Enum>
    constexpr Enum minAuthority(Enum lhs, Enum rhs)
    {
      return std::min(lhs, rhs);
    }
  } // namespace

  AuthorityPolicy intersectAuthority(AuthorityPolicy const& agent,
                                     AuthorityPolicy const& binding,
                                     AuthorityPolicy const& engine)
  {
    return AuthorityPolicy{
      .id = std::format("{}&{}&{}", agent.id, binding.id, engine.id),
      .filesystem = minAuthority(minAuthority(agent.filesystem, binding.filesystem), engine.filesystem),
      .network = minAuthority(minAuthority(agent.network, binding.network), engine.network),
      .contextView = minAuthority(minAuthority(agent.contextView, binding.contextView), engine.contextView),
    };
  }

  std::string makePhaseId()
  {
    static auto counter = std::atomic_uint64_t{};
    auto const now = std::chrono::system_clock::now().time_since_epoch();
    auto const micros = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
    return std::format("phase-{}-{}", micros, counter.fetch_add(1, std::memory_order_relaxed));
  }

  std::string RouteKey::canonical() const
  {
    return std::format("{}|{}|{}|{}|{}|{}|{}|{}",
                       agentId,
                       modelVersion,
                       harness,
                       toString(engine),
                       oracleId,
                       oracleVersion,
                       authority,
                       scopeRiskClass);
  }

  std::string_view toString(ScopeOperation value)
  {
    switch (value)
    {
      case ScopeOperation::Create: return "create";
      case ScopeOperation::Modify: return "modify";
      case ScopeOperation::Delete: return "delete";
    }

    return "invalid";
  }

  std::string_view toString(EngineKind value)
  {
    switch (value)
    {
      case EngineKind::Gate: return "gate";
      case EngineKind::Synthesis: return "synthesis";
      case EngineKind::Search: return "search";
    }

    return "invalid";
  }

  std::string_view toString(OutputMode value)
  {
    switch (value)
    {
      case OutputMode::Proposal: return "proposal";
      case OutputMode::Advisory: return "advisory";
    }

    return "invalid";
  }

  std::string_view toString(FailureReason value)
  {
    switch (value)
    {
      case FailureReason::None: return "none";
      case FailureReason::NoCandidate: return "no-candidate";
      case FailureReason::ScopeViolation: return "scope-violation";
      case FailureReason::ChurnExceeded: return "churn-exceeded";
      case FailureReason::OracleFailed: return "oracle-failed";
      case FailureReason::RiskOracleFired: return "risk-oracle-fired";
      case FailureReason::BudgetExhausted: return "budget-exhausted";
      case FailureReason::Infrastructure: return "infrastructure";
      case FailureReason::DependencyFailed: return "dependency-failed";
      case FailureReason::RealTreeChanged: return "real-tree-changed";
      case FailureReason::QuorumFailed: return "quorum-failed";
      case FailureReason::RoutePaused: return "route-paused";
    }

    return "invalid";
  }

  std::string_view toString(FilesystemAuthority value)
  {
    switch (value)
    {
      case FilesystemAuthority::ReadOnly: return "read-only";
      case FilesystemAuthority::WritableCopy: return "writable-copy";
      case FilesystemAuthority::MutateRealTree: return "mutate-real-tree";
    }

    return "invalid";
  }

  std::string_view toString(NetworkAuthority value)
  {
    switch (value)
    {
      case NetworkAuthority::Off: return "off";
      case NetworkAuthority::Vendor: return "vendor";
      case NetworkAuthority::Full: return "full";
    }

    return "invalid";
  }

  std::string_view toString(ContextView value)
  {
    switch (value)
    {
      case ContextView::Minimal: return "minimal";
      case ContextView::Full: return "full";
    }

    return "invalid";
  }

  std::string_view toString(OracleRunner value)
  {
    switch (value)
    {
      case OracleRunner::TestAll: return "test-all";
      case OracleRunner::TestCore: return "test-core";
      case OracleRunner::TestGtk: return "test-gtk";
      case OracleRunner::TidyClean: return "tidy-clean";
      case OracleRunner::BuildDebug: return "build-debug";
      case OracleRunner::TestDelta: return "test-delta";
      case OracleRunner::PublicSignatureDelta: return "public-signature-delta";
    }

    return "invalid";
  }

  std::string_view toString(BaselinePolicy value)
  {
    switch (value)
    {
      case BaselinePolicy::RequireGreen: return "require-green";
      case BaselinePolicy::AllowRed: return "allow-red";
      case BaselinePolicy::Skip: return "skip";
    }

    return "invalid";
  }

  std::string_view toString(EscalationAction value)
  {
    switch (value)
    {
      case EscalationAction::Retry: return "retry";
      case EscalationAction::SwitchRoute: return "switch-route";
      case EscalationAction::RequireCouncil: return "require-council";
      case EscalationAction::StopRoute: return "stop-route";
      case EscalationAction::ReturnChair: return "return-chair";
    }

    return "invalid";
  }

  std::string_view toString(CouncilDepth value)
  {
    switch (value)
    {
      case CouncilDepth::Panel: return "panel";
      case CouncilDepth::Challenge: return "challenge";
      case CouncilDepth::Full: return "full";
    }

    return "invalid";
  }

  std::string_view toString(ReviewVerdict value)
  {
    switch (value)
    {
      case ReviewVerdict::Accept: return "accept";
      case ReviewVerdict::Modify: return "modify";
      case ReviewVerdict::Reject: return "reject";
    }

    return "invalid";
  }

  std::string_view toString(ProcessStatus value)
  {
    switch (value)
    {
      case ProcessStatus::Exited: return "exited";
      case ProcessStatus::Signaled: return "signaled";
      case ProcessStatus::TimedOut: return "timed-out";
      case ProcessStatus::LaunchFailed: return "launch-failed";
    }

    return "invalid";
  }
} // namespace ao::fleet
