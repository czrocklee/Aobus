// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "PerformanceReport.h"

#include <ao/utility/Path.h>

#include <unicode/uvernum.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <ios>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace ao::rt::test
{
  namespace
  {
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
