// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt::test
{
  struct BaselineMetric final
  {
    std::string name;
    std::int64_t value = 0;
    std::string unit;
  };

  struct BaselineRecord final
  {
    std::string benchmark;
    std::vector<BaselineMetric> metrics;
  };

  void recordBaseline(std::string benchmark, std::vector<BaselineMetric> metrics);
  Result<> writeBaselineReport(std::ostream& output, std::span<BaselineRecord const> records);
  Result<> writeBaselineReport(std::filesystem::path const& path, std::span<BaselineRecord const> records);
  Result<> writeRequestedBaselineReport();

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
