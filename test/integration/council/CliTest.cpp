// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "council/CouncilSchema.h"
#include "council/ProcessRunner.h"
#include "test/council/TestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace ao::council::test
{
  namespace
  {
    std::filesystem::path councilExe()
    {
      return std::filesystem::path{AOBUS_COUNCIL_EXE};
    }

    std::filesystem::path productionRegistry()
    {
      return std::filesystem::path{AOBUS_SOURCE_DIR} / "config" / "agent-council.yaml";
    }

    ProcessResult runCouncil(std::vector<std::string> args)
    {
      args.insert(args.begin(), councilExe().string());
      auto runner = BoostProcessRunner{};
      return runner.run(ProcessRequest{
        .argv = std::move(args),
        .cwd = std::filesystem::path{AOBUS_SOURCE_DIR},
        .standardInput = {},
        .environmentWhitelist = {"PATH"},
        .environment = {},
        .timeout = std::chrono::seconds{20},
        .terminationGracePeriod = std::chrono::seconds{1},
      });
    }
  } // namespace

  TEST_CASE("Council CLI - validates the production registry", "[council][integration][cli]")
  {
    auto result = runCouncil({"validate-config", "--registry", productionRegistry().string()});

    REQUIRE(result.status == ProcessStatus::Exited);
    CHECK(result.exitCode == 0);
    CHECK(result.standardOutput.find("registry valid") != std::string::npos);
  }

  TEST_CASE("Council CLI - configuration errors use the configuration exit code", "[council][integration][cli]")
  {
    auto temp = ao::test::TempDir{};
    auto const registry = writeFile(temp,
                                    "registry.yaml",
                                    R"(schema: wrong/v1
harnesses: {}
agents: {}
councils: {}
)");

    auto result = runCouncil({"validate-config", "--registry", registry.string()});

    REQUIRE(result.status == ProcessStatus::Exited);
    CHECK(result.exitCode == 5);
    CHECK(result.standardError.find("invalid registry schema") != std::string::npos);
  }

  TEST_CASE("Council CLI - intent input errors use the CLI exit code", "[council][integration][cli]")
  {
    auto temp = ao::test::TempDir{};
    std::filesystem::create_directories(tempPath(temp) / "repo");
    auto const intent = writeFile(temp,
                                  "intent.yaml",
                                  R"(schema: wrong/v1
id: phase-a
task-kind: council-review
invariant: Preserve behavior.
depends-on: []
overrides: {}
body: |
  Review the change.
)");

    auto result = runCouncil({"run",
                              "--registry",
                              productionRegistry().string(),
                              "--repo",
                              (tempPath(temp) / "repo").string(),
                              "--out",
                              (tempPath(temp) / "out").string(),
                              intent.string()});

    REQUIRE(result.status == ProcessStatus::Exited);
    CHECK(result.exitCode == 64);
    CHECK(result.standardError.find("invalid intent schema") != std::string::npos);
  }

  TEST_CASE("Council CLI - resolved intent errors use the CLI exit code", "[council][integration][cli]")
  {
    auto temp = ao::test::TempDir{};
    std::filesystem::create_directories(tempPath(temp) / "repo");
    auto const intent = writeFile(temp,
                                  "intent.yaml",
                                  R"(schema: aobus-council-intent/v1
id: phase-a
task-kind: missing-council
invariant: Preserve behavior.
depends-on: []
overrides: {}
body: |
  Review the change.
)");

    auto result = runCouncil({"run",
                              "--registry",
                              productionRegistry().string(),
                              "--repo",
                              (tempPath(temp) / "repo").string(),
                              "--out",
                              (tempPath(temp) / "out").string(),
                              intent.string()});

    REQUIRE(result.status == ProcessStatus::Exited);
    CHECK(result.exitCode == 64);
    CHECK(result.standardError.find("no definition for task-kind 'missing-council'") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(tempPath(temp) / "out"));
  }

  TEST_CASE("Council CLI - policy errors use the policy exit code", "[council][integration][cli]")
  {
    auto temp = ao::test::TempDir{};
    std::filesystem::create_directories(tempPath(temp) / "repo");
    auto const intent = writeFile(temp, "intent.yaml", intentYaml());

    auto result = runCouncil({"run",
                              "--registry",
                              productionRegistry().string(),
                              "--repo",
                              (tempPath(temp) / "repo").string(),
                              "--out",
                              (tempPath(temp) / "repo" / "out").string(),
                              intent.string()});

    REQUIRE(result.status == ProcessStatus::Exited);
    CHECK(result.exitCode == 2);
    CHECK(result.standardError.find("must be outside repository") != std::string::npos);
  }

  TEST_CASE("Council CLI - infrastructure errors use the infrastructure exit code", "[council][integration][cli]")
  {
    auto temp = ao::test::TempDir{};
    std::filesystem::create_directories(tempPath(temp) / "repo");
    auto const intent = writeFile(temp, "intent.yaml", intentYaml());

    auto result = runCouncil({"run",
                              "--registry",
                              productionRegistry().string(),
                              "--repo",
                              (tempPath(temp) / "repo").string(),
                              "--out",
                              "/dev/null/aobus-council-out",
                              intent.string()});

    REQUIRE(result.status == ProcessStatus::Exited);
    CHECK(result.exitCode == 3);
  }
} // namespace ao::council::test
