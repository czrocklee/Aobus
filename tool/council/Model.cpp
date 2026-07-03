// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "council/Model.h"

#include <format>
#include <random>
#include <string>
#include <string_view>

namespace ao::council
{
  std::string makePhaseId()
  {
    static auto generator = std::mt19937_64{std::random_device{}()};
    return std::format("phase-{:016x}", generator());
  }

  std::string_view toString(FailureReason value)
  {
    return enumName(kFailureReasonNames, value);
  }

  std::string_view toString(PromptDelivery value)
  {
    return enumName(kPromptDeliveryNames, value);
  }

  std::string_view toString(Depth value)
  {
    return enumName(kDepthNames, value);
  }

  std::string_view toString(ProcessStatus value)
  {
    return enumName(kProcessStatusNames, value);
  }
} // namespace ao::council
