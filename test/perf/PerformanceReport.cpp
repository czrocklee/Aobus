// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "PerformanceReport.h"

#include <ao/Error.h>
#include <ao/utility/Path.h>

#include <unicode/uvernum.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    std::vector<BaselineRecord>& baselineRecords()
    {
      static auto records = std::vector<BaselineRecord>{};
      return records;
    }

    Result<> validateBaselineRecords(std::span<BaselineRecord const> const records)
    {
      if (records.empty())
      {
        return makeError(Error::Code::InvalidInput, "Requested baseline report has no records");
      }

      for (auto const& record : records)
      {
        if (record.benchmark.empty() || record.metrics.empty())
        {
          return makeError(Error::Code::InvalidInput, "Baseline records require a benchmark and metrics");
        }

        for (auto const& metric : record.metrics)
        {
          if (metric.name.empty() || metric.unit.empty())
          {
            return makeError(Error::Code::InvalidInput, "Baseline metrics require a name and unit");
          }
        }
      }

      return {};
    }

    std::string jsonEscape(std::string_view const value)
    {
      auto result = std::string{};
      result.reserve(value.size() + 8);

      for (auto const ch : value)
      {
        switch (ch)
        {
          case '"': result += "\\\""; break;
          case '\\': result += "\\\\"; break;
          case '\n': result += "\\n"; break;
          case '\r': result += "\\r"; break;
          case '\t': result += "\\t"; break;
          default:
            if (static_cast<unsigned char>(ch) < 0x20U)
            {
              result += std::format("\\u{:04x}", static_cast<unsigned char>(ch));
            }
            else
            {
              result += ch;
            }

            break;
        }
      }

      return result;
    }
  } // namespace

  void recordBaseline(std::string benchmark, std::vector<BaselineMetric> metrics)
  {
    baselineRecords().push_back(BaselineRecord{.benchmark = std::move(benchmark), .metrics = std::move(metrics)});
  }

  Result<> writeBaselineReport(std::ostream& output, std::span<BaselineRecord const> const records)
  {
    if (auto validationRes = validateBaselineRecords(records); !validationRes)
    {
      return validationRes;
    }

    try
    {
      output << "{\n  \"schema\": \"aobus-performance-baseline/v1\",\n  \"records\": [\n";

      for (std::size_t index = 0; index < records.size(); ++index)
      {
        auto const& record = records[index];
        output << R"(    {"benchmark": ")" << jsonEscape(record.benchmark) << R"(", "metrics": [)" << '\n';

        for (std::size_t metricIndex = 0; metricIndex < record.metrics.size(); ++metricIndex)
        {
          auto const& metric = record.metrics[metricIndex];
          output << R"(      {"name": ")" << jsonEscape(metric.name) << R"(", "value": )" << metric.value
                 << R"(, "unit": ")" << jsonEscape(metric.unit) << R"("})"
                 << (metricIndex + 1 == record.metrics.size() ? "\n" : ",\n");
        }

        output << "    ]}" << (index + 1 == records.size() ? "\n" : ",\n");
      }

      output << "  ]\n}\n";
      output.flush();
    }
    catch (std::ios_base::failure const&)
    {
      return makeError(Error::Code::IoError, "Could not write or flush baseline report");
    }

    if (!output)
    {
      return makeError(Error::Code::IoError, "Could not write or flush baseline report");
    }

    return {};
  }

  Result<> writeBaselineReport(std::filesystem::path const& path, std::span<BaselineRecord const> const records)
  {
    if (auto validationRes = validateBaselineRecords(records); !validationRes)
    {
      return validationRes;
    }

    auto output = std::ofstream{path, std::ios::out | std::ios::binary | std::ios::noreplace};

    if (!output)
    {
      return makeError(Error::Code::IoError, "Could not create baseline report; use a fresh output path");
    }

    auto writeRes = writeBaselineReport(output, records);
    output.close();

    if (!writeRes)
    {
      return writeRes;
    }

    if (!output)
    {
      return makeError(Error::Code::IoError, "Could not close baseline report");
    }

    return {};
  }

  Result<> writeRequestedBaselineReport()
  {
    auto const path = environmentText("AOBUS_PERF_BASELINE_JSON", "");

    if (path.empty())
    {
      return {};
    }

    return writeBaselineReport(utility::pathFromUtf8(path), baselineRecords());
  }

  std::string environmentText(char const* const name, std::string_view const fallback)
  {
    auto const* const value = std::getenv(name);
    return value == nullptr || value[0] == '\0' ? std::string{fallback} : value;
  }

  std::size_t configuredCount(char const* const name, std::size_t const fallback, std::size_t const minimum)
  {
    auto const* const raw = std::getenv(name);

    if (raw == nullptr || raw[0] == '\0')
    {
      return fallback;
    }

    std::size_t parsed = 0;
    auto const* const limit = raw + std::strlen(raw);
    auto const [end, error] = std::from_chars(raw, limit, parsed);

    if (error != std::errc{} || end != limit || parsed < minimum)
    {
      throw std::runtime_error{std::format("{} is outside the supported range", name)};
    }

    return parsed;
  }

  void writeReport(std::span<Measurement const> const measurements,
                   std::size_t const warmups,
                   std::size_t const samples)
  {
    auto const* const outputPath = std::getenv("AOBUS_PERF_REPORT_JSON");

    if (outputPath == nullptr || outputPath[0] == '\0')
    {
      return;
    }

    auto output = std::ofstream{utility::pathFromUtf8(outputPath), std::ios::out | std::ios::noreplace};

    if (!output)
    {
      throw std::runtime_error{std::format(
        "could not create performance report '{}'; select one reporting test case per output and use a fresh path",
        outputPath)};
    }

    output << '{' << '\n';
    output << R"(  "schema": "aobus-performance-review/v2",)" << '\n';
    output << R"(  "metadata": {)" << '\n';
    output << R"(    "revision": ")" << jsonEscape(environmentText("AOBUS_PERF_REVISION")) << R"(",)" << '\n';
    output << R"(    "compiler": ")" << jsonEscape(environmentText("AOBUS_PERF_COMPILER")) << R"(",)" << '\n';
    output << R"(    "build_mode": ")" << jsonEscape(environmentText("AOBUS_PERF_BUILD_MODE")) << R"(",)" << '\n';
    output << R"(    "platform": ")" << jsonEscape(environmentText("AOBUS_PERF_PLATFORM")) << R"(",)" << '\n';
    output << R"(    "icu_version": ")" << U_ICU_VERSION << R"(",)" << '\n';
    output << R"(    "warmups": )" << warmups << ',' << '\n';
    output << R"(    "samples": )" << samples << '\n';
    output << "  }," << '\n';
    output << R"(  "measurements": [)" << '\n';

    for (std::size_t index = 0; index < measurements.size(); ++index)
    {
      auto const& item = measurements[index];
      output << R"(    {"capability": ")" << jsonEscape(item.capability) << R"(", )"
             << R"("scenario": ")" << jsonEscape(item.scenario) << R"(", )"
             << R"("dataset": ")" << jsonEscape(item.dataset) << R"(", )"
             << R"("input_count": )" << item.inputCount << ", "
             << R"("median_ns": )" << item.medianNs << ", "
             << R"("p95_ns": )" << item.percentile95Ns;

      if (item.optPolicy)
      {
        output << R"(, "policy": ")" << jsonEscape(*item.optPolicy) << '"';
      }

      if (item.optLocale)
      {
        output << R"(, "locale": ")" << jsonEscape(*item.optLocale) << '"';
      }

      if (item.optByteMetric)
      {
        output << R"(, "byte_metric": {"kind": ")" << jsonEscape(item.optByteMetric->kind) << R"(", "count": )"
               << item.optByteMetric->count << '}';
      }

      output << '}';
      output << (index + 1 == measurements.size() ? "\n" : ",\n");
    }

    output << "  ]" << '\n';
    output << '}' << '\n';

    output.close();

    if (!output)
    {
      throw std::runtime_error{std::format("could not write performance report '{}'", outputPath)};
    }
  }

  void setPercentiles(Measurement& measurement, std::span<std::int64_t> const elapsed)
  {
    if (elapsed.empty())
    {
      throw std::runtime_error{"performance measurement has no samples"};
    }

    std::ranges::sort(elapsed);
    measurement.medianNs = elapsed[elapsed.size() / 2];
    measurement.percentile95Ns = elapsed[(((elapsed.size() * 95U) + 99U) / 100U) - 1U];
  }
} // namespace ao::rt::test
