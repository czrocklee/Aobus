// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "council/Serialization.h"
#include "test/council/TestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <fstream>
#include <ios>
#include <string>
#include <thread>
#include <vector>

namespace ao::council::test
{
  TEST_CASE("YAML stream - control bytes in scalars round trip", "[council][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto const path = tempPath(temp) / "stream.yaml";
    auto const hostile = std::string{"esc\x1b[31m bell\x07 del\x7f raw\xff end"};

    auto document = std::string{"schema: aobus-council-trace-event/v1\nvalue: "};
    document += yamlScalar(hostile);
    document += "\n";
    REQUIRE(appendYamlDocument(path, document));

    auto stream = readScalarStream(path, "aobus-council-trace-event/v1");
    REQUIRE(stream);
    CHECK_FALSE(stream->trailingCorruption);
    REQUIRE(stream->documents.size() == 1);
    CHECK(stream->documents.front().at("value") == hostile);
  }

  TEST_CASE("YAML stream - concurrent appends never interleave documents", "[council][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto const path = tempPath(temp) / "trace.yaml";
    constexpr std::size_t kThreads = 8;
    constexpr std::size_t kDocumentsPerThread = 25;
    auto workers = std::vector<std::thread>{};
    auto appended = std::array<bool, kThreads>{};

    for (std::size_t worker = 0; worker < kThreads; ++worker)
    {
      workers.emplace_back(
        [&path, &appended, worker]
        {
          bool success = true;

          for (std::size_t document = 0; document < kDocumentsPerThread; ++document)
          {
            auto const payload =
              std::format("schema: aobus-council-trace-event/v1\nvalue: \"w{}-d{}\"\n", worker, document);
            success = success && appendYamlDocument(path, payload).has_value();
          }

          appended[worker] = success;
        });
    }

    for (auto& thread : workers)
    {
      thread.join();
    }

    CHECK(std::ranges::all_of(appended, [](bool value) { return value; }));

    auto stream = readScalarStream(path, "aobus-council-trace-event/v1");
    REQUIRE(stream);
    CHECK_FALSE(stream->trailingCorruption);
    CHECK(stream->documents.size() == kThreads * kDocumentsPerThread);
  }

  TEST_CASE("YAML stream - CRLF document separators split documents", "[council][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto const path = tempPath(temp) / "trace.yaml";

    {
      auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
      output << "schema: aobus-council-trace-event/v1\r\n";
      output << "event: first\r\n";
      output << "---\r\n";
      output << "schema: aobus-council-trace-event/v1\r\n";
      output << "event: second\r\n";
    }

    auto stream = readScalarStream(path, "aobus-council-trace-event/v1");
    REQUIRE(stream);
    CHECK_FALSE(stream->trailingCorruption);
    REQUIRE(stream->documents.size() == 2);
    CHECK(stream->documents[0].at("event") == "first");
    CHECK(stream->documents[1].at("event") == "second");
  }

  TEST_CASE("YAML stream - complete structured fields are rejected", "[council][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto const path = tempPath(temp) / "trace.yaml";

    {
      auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
      output << "schema: aobus-council-trace-event/v1\n";
      output << "event: phase-started\n";
      output << "details:\n";
      output << "  phase-id: phase-a\n";
    }

    auto stream = readScalarStream(path, "aobus-council-trace-event/v1");
    REQUIRE_FALSE(stream);
    CHECK(stream.error().message.find("stream field 'details' must be a scalar") != std::string::npos);
  }

  TEST_CASE("YAML stream - incomplete tail is recoverable", "[council][unit][yaml]")
  {
    auto temp = ao::test::TempDir{};
    auto const path = tempPath(temp) / "trace.yaml";
    REQUIRE(appendYamlDocument(path, emitTraceEvent("phase-started", {{"phase-id", "phase-a"}})));

    {
      auto output = std::ofstream{path, std::ios::app | std::ios::binary};
      output << "---\nschema: aobus-council-trace-event/v1\nphase-id:";
    }

    auto result = readScalarStream(path, "aobus-council-trace-event/v1");

    REQUIRE(result);
    REQUIRE(result->documents.size() == 1);
    CHECK(result->documents.front().at("event") == "phase-started");
    CHECK(result->trailingCorruption);
  }
} // namespace ao::council::test
