// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <span>
#include <string_view>

namespace ao::test
{
  struct FatalProbeExpectation final
  {
    constexpr FatalProbeExpectation(std::string_view scenarioValue,
                                    std::string_view requiredMarkerValue,
                                    std::string_view secondRequiredMarkerValue,
                                    std::string_view forbiddenMarkerValue,
                                    bool emergencyBeforeSinkValue = false,
                                    std::string_view sourceMarkerValue = "FatalProbeScenario.cpp:",
                                    std::string_view thirdRequiredMarkerValue = {}) noexcept
      : scenario{scenarioValue}
      , requiredMarker{requiredMarkerValue}
      , secondRequiredMarker{secondRequiredMarkerValue}
      , forbiddenMarker{forbiddenMarkerValue}
      , emergencyBeforeSink{emergencyBeforeSinkValue}
      , sourceMarker{sourceMarkerValue}
      , thirdRequiredMarker{thirdRequiredMarkerValue}
    {
    }

    std::string_view scenario;
    std::string_view requiredMarker;
    std::string_view secondRequiredMarker;
    std::string_view forbiddenMarker;
    bool emergencyBeforeSink = false;
    std::string_view sourceMarker = "FatalProbeScenario.cpp:";
    std::string_view thirdRequiredMarker;
  };

  std::span<FatalProbeExpectation const> fatalProbeExpectations() noexcept;
} // namespace ao::test
