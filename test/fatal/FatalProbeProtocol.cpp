// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/fatal/FatalProbeProtocol.h"

#include <array>
#include <span>

namespace ao::test
{
  std::span<FatalProbeExpectation const> fatalProbeExpectations() noexcept
  {
    static constexpr auto kExpectations = std::array{
      FatalProbeExpectation{"expects",
                            "category=expects",
                            "sink=unavailable",
                            {},
                            false,
                            "FatalProbeScenario.cpp:",
                            "context=evaluation count 1"},
      FatalProbeExpectation{"duplicate-prepared-next",
                            "category=expects",
                            "Prepared-next request must be cleared before preparing a replacement",
                            "PreparedNextContract"},
      FatalProbeExpectation{"ensures", "category=ensures", "source=", {}},
      FatalProbeExpectation{"invariant", "category=invariant", "source=", {}},
      FatalProbeExpectation{"formatted-fatal", "category=fatal", "context=formatted value 42", {}},
      FatalProbeExpectation{
        "realtime-invariant", "category=realtime-invariant", "context=realtime probe", "AOBUS_TEST sink=accepted"},
      FatalProbeExpectation{"accepted-sink", "category=fatal", "AOBUS_TEST sink=accepted", "sink=rejected", true},
      FatalProbeExpectation{"rejected-sink", "category=fatal", "sink=rejected", {}},
      FatalProbeExpectation{"throwing-sink", "sink=exception", "sink=rejected", {}},
      FatalProbeExpectation{"recursive-sink", "category=fatal", "category=recursive-fatal", {}},
      FatalProbeExpectation{
        "unhandled-exception", "category=unhandled-exception", "context=probe root: probe exception", {}},
      FatalProbeExpectation{
        "unhandled-unknown-exception", "category=unhandled-exception", "context=unknown root: Unknown exception", {}},
      FatalProbeExpectation{
        "missing-exception", "category=unhandled-exception", "context=missing root: Missing exception payload", {}},
      FatalProbeExpectation{"truncated-context", "category=fatal", "context-truncated=true", {}},
      FatalProbeExpectation{"truncated-diagnostic", "category=fatal", " diagnostic-truncated=true", {}},
      FatalProbeExpectation{
        "throwing-condition", "category=expects", "Fatal condition or context evaluation threw", {}},
      FatalProbeExpectation{"throwing-context", "category=fatal", "Fatal condition or context evaluation threw", {}},
      FatalProbeExpectation{"concurrent-entry",
                            "context=first concurrent fatal",
                            "context=second concurrent fatal",
                            {},
                            false,
                            "FatalProbeScenario.cpp:",
                            "category=concurrent-fatal"},
    };

    return kExpectations;
  }
} // namespace ao::test
