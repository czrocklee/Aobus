// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ao::rt::test
{
  struct Measurement final
  {
    struct ByteMetric final
    {
      std::string kind;
      std::size_t count = 0;
    };

    std::string capability;
    std::optional<std::string> optPolicy = std::nullopt;
    std::string scenario;
    std::optional<std::string> optLocale = std::nullopt;
    std::string dataset;
    std::size_t inputCount = 0;
    std::int64_t medianNs = 0;
    std::int64_t percentile95Ns = 0;
    std::optional<ByteMetric> optByteMetric = std::nullopt;
  };

  std::string environmentText(char const* name, std::string_view fallback = "unknown");
  std::size_t configuredCount(char const* name, std::size_t fallback, std::size_t minimum);
  void setPercentiles(Measurement& measurement, std::span<std::int64_t> elapsed);
  void writeReport(std::span<Measurement const> measurements, std::size_t warmups, std::size_t samples);
} // namespace ao::rt::test
