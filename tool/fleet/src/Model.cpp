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

  std::string_view toString(FilesystemAuthority value)
  {
    return enumName(kFilesystemAuthorityNames, value);
  }

  std::string_view toString(NetworkAuthority value)
  {
    return enumName(kNetworkAuthorityNames, value);
  }

  std::string_view toString(ContextView value)
  {
    return enumName(kContextViewNames, value);
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
