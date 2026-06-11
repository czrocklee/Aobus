// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/fleet/Model.h>

#include <atomic>
#include <chrono>
#include <format>
#include <string>
#include <string_view>

namespace ao::fleet
{
  std::string makePhaseId()
  {
    static auto counter = std::atomic_uint64_t{};
    auto const now = std::chrono::system_clock::now().time_since_epoch();
    auto const micros = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
    return std::format("phase-{}-{}", micros, counter.fetch_add(1, std::memory_order_relaxed));
  }

  std::string RouteKey::canonical() const
  {
    return std::format("{}|{}|{}|{}|{}|{}|{}",
                       agentId,
                       modelVersion,
                       harness,
                       toString(engine),
                       oracleId,
                       oracleVersion,
                       scopeRiskClass);
  }

  std::string_view toString(ScopeOperation value)
  {
    return enumName(kScopeOperationNames, value);
  }

  std::string_view toString(EngineKind value)
  {
    return enumName(kEngineKindNames, value);
  }

  std::string_view toString(OutputMode value)
  {
    return enumName(kOutputModeNames, value);
  }

  std::string_view toString(FailureReason value)
  {
    return enumName(kFailureReasonNames, value);
  }

  std::string_view toString(PromptDelivery value)
  {
    return enumName(kPromptDeliveryNames, value);
  }

  std::string_view toString(OracleRunner value)
  {
    return enumName(kOracleRunnerNames, value);
  }

  std::string_view toString(BaselinePolicy value)
  {
    return enumName(kBaselinePolicyNames, value);
  }

  std::string_view toString(EscalationAction value)
  {
    return enumName(kEscalationActionNames, value);
  }

  std::string_view toString(CouncilDepth value)
  {
    return enumName(kCouncilDepthNames, value);
  }

  std::string_view toString(ReviewVerdict value)
  {
    return enumName(kReviewVerdictNames, value);
  }

  std::string_view toString(ProcessStatus value)
  {
    return enumName(kProcessStatusNames, value);
  }
} // namespace ao::fleet
