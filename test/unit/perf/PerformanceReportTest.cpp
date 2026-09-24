// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/perf/PerformanceReport.h"

#include "test/unit/TestFixtureSupport.h"
#include <ao/Error.h>
#include <ao/utility/Path.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <ios>
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace ao::rt::test
{
  namespace
  {
    std::array<BaselineRecord, 2> sampleBaselineRecords()
    {
      return {
        BaselineRecord{.benchmark = "phase \"one\"\n", .metrics = {{"elapsed\\path", 7, "us"}, {"count", 0, "item"}}},
        BaselineRecord{.benchmark = "phase two", .metrics = {{"memory\tchange", -3, "KiB"}}}};
    }

    constexpr auto kExpectedBaseline = R"({
  "schema": "aobus-performance-baseline/v1",
  "records": [
    {"benchmark": "phase \"one\"\n", "metrics": [
      {"name": "elapsed\\path", "value": 7, "unit": "us"},
      {"name": "count", "value": 0, "unit": "item"}
    ]},
    {"benchmark": "phase two", "metrics": [
      {"name": "memory\tchange", "value": -3, "unit": "KiB"}
    ]}
  ]
}
)";

    class FailingBuffer final : public std::stringbuf
    {
    public:
      explicit FailingBuffer(bool failWrites)
        : _failWrites{failWrites}
      {
      }
      bool hasFlushed() const { return _flushed; }

    protected:
      std::streamsize xsputn(char const* bytes, std::streamsize count) override
      {
        return _failWrites ? 0 : std::stringbuf::xsputn(bytes, count);
      }

      int sync() override
      {
        _flushed = true;
        return -1;
      }

    private:
      bool _failWrites;
      bool _flushed = false;
    };
  } // namespace

  TEST_CASE("PerformanceReport - baseline JSON preserves records and escaped metric values", "[perf][unit][report]")
  {
    auto const records = sampleBaselineRecords();
    auto output = std::ostringstream{};
    REQUIRE(writeBaselineReport(output, records));
    CHECK(output.str() == kExpectedBaseline);
  }

  TEST_CASE("PerformanceReport - incomplete baseline data cannot publish output", "[perf][unit][report]")
  {
    auto records = std::vector{BaselineRecord{.benchmark = "phase", .metrics = {{"count", 1, "item"}}}};

    SECTION("no records")
    {
      records.clear();
    }

    SECTION("empty benchmark")
    {
      records[0].benchmark.clear();
    }

    SECTION("no metrics")
    {
      records[0].metrics.clear();
    }

    SECTION("empty metric name")
    {
      records[0].metrics[0].name.clear();
    }

    SECTION("empty metric unit")
    {
      records[0].metrics[0].unit.clear();
    }

    auto output = std::ostringstream{};
    output << "sentinel";
    auto const reportRes = writeBaselineReport(output, records);
    REQUIRE_FALSE(reportRes);
    CHECK(reportRes.error().code == Error::Code::InvalidInput);
    CHECK(output.str() == "sentinel");
  }

  TEST_CASE("PerformanceReport - baseline files require a fresh writable destination", "[perf][unit][report]")
  {
    auto const directory = ao::test::TempDir{};
    auto const path = directory.path() / utility::pathFromUtf8("报告.json");
    auto const records = sampleBaselineRecords();
    REQUIRE(writeBaselineReport(path, records));
    CHECK(ao::test::readFile(path) == kExpectedBaseline);

    CHECK_FALSE(writeBaselineReport(path, records));
    CHECK(ao::test::readFile(path) == kExpectedBaseline);
    CHECK_FALSE(writeBaselineReport(path, std::span<BaselineRecord const>{}));
    CHECK(ao::test::readFile(path) == kExpectedBaseline);
    CHECK_FALSE(writeBaselineReport(directory.path(), records));
    CHECK(std::filesystem::is_directory(directory.path()));
    CHECK_FALSE(writeBaselineReport(directory.path() / "absent" / "report.json", records));
    CHECK_FALSE(std::filesystem::exists(directory.path() / "absent"));

    auto const emptyPath = directory.path() / "empty.json";
    CHECK_FALSE(writeBaselineReport(emptyPath, std::span<BaselineRecord const>{}));
    CHECK_FALSE(std::filesystem::exists(emptyPath));
  }

  TEST_CASE("PerformanceReport - insertion and flush failures reject the baseline", "[perf][unit][report]")
  {
    auto const records = sampleBaselineRecords();

    for (bool const failWrites : {true, false})
    {
      for (bool const throwFailures : {true, false})
      {
        auto buffer = FailingBuffer{failWrites};
        auto output = std::ostream{&buffer};

        if (throwFailures)
        {
          output.exceptions(std::ios::badbit | std::ios::failbit);
        }

        auto const reportRes = writeBaselineReport(output, records);
        REQUIRE_FALSE(reportRes);
        CHECK(reportRes.error().code == Error::Code::IoError);

        if (!failWrites)
        {
          CHECK(buffer.str() == kExpectedBaseline);
          CHECK(buffer.hasFlushed());
        }
      }
    }
  }
} // namespace ao::rt::test
