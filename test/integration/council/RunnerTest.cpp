// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "council/Engine.h"
#include "council/Model.h"
#include "council/ProcessRunner.h"
#include "test/council/TestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace ao::council::test
{
  namespace
  {
    struct FakeRunner final : ProcessRunner
    {
      std::vector<ProcessRequest> requests;
      std::map<std::string, ProcessResult, std::less<>> agentResults;
      std::map<std::pair<std::string, std::string>, ProcessResult, std::less<>> roundAgentResults;
      std::filesystem::path mutateRealRepoPath = {};
      std::mutex mutex;
      std::condition_variable concurrentReady;
      std::size_t requiredConcurrentAgents = 0;
      std::size_t activeAgents = 0;
      std::size_t maxActiveAgents = 0;
      bool releaseConcurrentAgents = false;

      ProcessResult run(ProcessRequest const& request) override
      {
        {
          auto const lock = std::scoped_lock{mutex};
          requests.push_back(request);
        }

        if (request.onLaunch)
        {
          request.onLaunch(4242);
        }

        auto result = ProcessResult{.status = ProcessStatus::Exited, .exitCode = 0};

        if (request.argv.empty())
        {
          return ProcessResult{.standardError = "empty argv"};
        }

        if (request.argv.front() == "btrfs" && request.argv.size() >= 3 && request.argv[1] == "subvolume" &&
            request.argv[2] == "show")
        {
          result.exitCode = 1;
          return result;
        }

        if (request.argv.front() == "rsync" && request.argv.size() >= 2)
        {
          // Real rsync creates its destination; mirror that so snapshot/workspace directories exist on disk
          // and teardown has something concrete to reclaim.
          auto error = std::error_code{};
          std::filesystem::create_directories(request.argv.back(), error);
          return result;
        }

        if (request.argv.front() == "btrfs" || request.argv.front() == "git")
        {
          return result;
        }

        auto agentId = request.argv.front();
        auto roundId = std::string{};

        if (agentId == "bwrap")
        {
          for (std::size_t index = 0; index + 2 < request.argv.size(); ++index)
          {
            if (request.argv[index] == "--chdir")
            {
              agentId = request.argv[index + 2];
              break;
            }
          }

          if (request.cwd.filename() == "workspace")
          {
            roundId = request.cwd.parent_path().filename().string();
          }
        }

        if (requiredConcurrentAgents > 0 && (agentId == "member-a" || agentId == "member-b" || agentId == "member-c"))
        {
          auto lock = std::unique_lock{mutex};
          ++activeAgents;
          maxActiveAgents = std::max(maxActiveAgents, activeAgents);

          if (activeAgents >= requiredConcurrentAgents)
          {
            releaseConcurrentAgents = true;
            concurrentReady.notify_all();
          }
          else if (!concurrentReady.wait_for(lock, std::chrono::seconds{2}, [&] { return releaseConcurrentAgents; }))
          {
            --activeAgents;
            return ProcessResult{
              .status = ProcessStatus::Exited, .exitCode = 99, .standardError = "member did not run concurrently"};
          }

          --activeAgents;
        }

        {
          auto const lock = std::scoped_lock{mutex};

          if (!roundId.empty())
          {
            if (auto agent = roundAgentResults.find({agentId, roundId}); agent != roundAgentResults.end())
            {
              return agent->second;
            }
          }

          if (auto agent = agentResults.find(agentId); agent != agentResults.end())
          {
            return agent->second;
          }
        }

        auto mutatePath = std::filesystem::path{};

        {
          auto const lock = std::scoped_lock{mutex};
          mutatePath = mutateRealRepoPath;
          mutateRealRepoPath.clear();
        }

        if (!mutatePath.empty())
        {
          auto output = std::ofstream{mutatePath, std::ios::binary | std::ios::trunc};
          output << "mutated\n";
        }

        result.standardOutput = "response from " + agentId + "\n";
        return result;
      }
    };

    AgentDefinition agent(std::string id, std::string vendor)
    {
      return AgentDefinition{.id = id,
                             .harness = "fake",
                             .model = id + "-model",
                             .vendor = std::move(vendor),
                             .argvTemplate = {std::move(id)},
                             .promptDelivery = PromptDelivery::Stdin,
                             .environmentWhitelist = {"PATH"},
                             .timeout = std::chrono::seconds{5}};
    }

    Registry registry()
    {
      auto result = Registry{};
      result.agents.emplace("member-a", agent("member-a", "vendor-a"));
      result.agents.emplace("member-b", agent("member-b", "vendor-b"));
      result.agents.emplace("member-c", agent("member-c", "vendor-c"));
      result.councils.emplace(
        "council-review",
        Definition{.taskKind = "council-review",
                   .parameters = Parameters{
                     .roster = {"member-a", "member-b", "member-c"}, .depth = Depth::Challenge, .quorum = 2}});
      return result;
    }

    PhaseIntent intent(std::string id)
    {
      return PhaseIntent{.id = std::move(id),
                         .taskKind = "council-review",
                         .invariant = "Preserve behavior.",
                         .focus = {FocusRule{.path = "source.txt", .match = FocusMatch::Exact}},
                         .dependsOn = {},
                         .overrides = {},
                         .body = "Review the change."};
    }

    bool launchedAgent(FakeRunner const& runner, std::string const& id)
    {
      return std::ranges::any_of(
        runner.requests,
        [&](ProcessRequest const& request)
        { return request.argv.front() == "bwrap" && std::ranges::contains(request.argv, id); });
    }

    bool containsSequence(std::vector<std::string> const& argv, std::vector<std::string> const& sequence)
    {
      return std::ranges::search(argv, sequence).begin() != argv.end();
    }

    ProcessRequest const* memberRequest(FakeRunner const& runner, std::string const& id)
    {
      auto const request = std::ranges::find_if(
        runner.requests,
        [&](ProcessRequest const& item)
        { return !item.argv.empty() && item.argv.front() == "bwrap" && std::ranges::contains(item.argv, id); });
      return request == runner.requests.end() ? nullptr : &*request;
    }

    class [[nodiscard]] CurrentPathGuard final
    {
    public:
      explicit CurrentPathGuard(std::filesystem::path const& next)
        : _previous{std::filesystem::current_path()}
      {
        std::filesystem::current_path(next);
      }

      ~CurrentPathGuard() { std::filesystem::current_path(_previous); }

      CurrentPathGuard(CurrentPathGuard const&) = delete;
      CurrentPathGuard& operator=(CurrentPathGuard const&) = delete;
      CurrentPathGuard(CurrentPathGuard&&) = delete;
      CurrentPathGuard& operator=(CurrentPathGuard&&) = delete;

    private:
      std::filesystem::path _previous;
    };
  } // namespace

  TEST_CASE("Runner - full roster writes council artifacts", "[council][integration][engine]")
  {
    auto temp = ao::test::TempDir{};
    writeFile(temp, "repo/source.txt", "original\n");
    auto fake = FakeRunner{};
    auto run = Runner{fake}.run(registry(), {intent("phase-a")}, tempPath(temp) / "repo", tempPath(temp) / "out");

    REQUIRE(run);
    REQUIRE(run->manifests.size() == 1);
    CHECK(run->manifests.front().failure == FailureReason::None);
    CHECK_FALSE(run->failed);
    CHECK(launchedAgent(fake, "member-a"));
    CHECK(launchedAgent(fake, "member-b"));
    CHECK(launchedAgent(fake, "member-c"));

    auto const phase = tempPath(temp) / "out" / "phase-a";
    CHECK(std::filesystem::exists(phase / "dossier.md"));
    CHECK(std::filesystem::exists(phase / "manifest.yaml"));
    CHECK(std::filesystem::exists(phase / "evidence.yaml"));
    CHECK(std::filesystem::exists(phase / "members" / "member-a" / "r1" / "prompt.md"));
    CHECK(std::filesystem::exists(phase / "members" / "member-a" / "r2" / "prompt.md"));
    CHECK_FALSE(std::filesystem::exists(phase / "members" / "member-a" / "r3" / "prompt.md"));

    auto const evidence = readFile(phase / "evidence.yaml");
    CHECK(evidence.find("schema: aobus-council-evidence/v1") != std::string::npos);
    CHECK(evidence.find("round: r1") != std::string::npos);
    CHECK(evidence.find("round: r2") != std::string::npos);
    CHECK(evidence.find("agent: member-a") != std::string::npos);
    CHECK(evidence.find("members/member-a/r1/stdout.txt") != std::string::npos);
    CHECK(evidence.find("usable: true") != std::string::npos);

    auto const trace = readFile(phase / "trace.yaml");
    CHECK(trace.find("member-started") != std::string::npos);
    CHECK(trace.find("member-launched") != std::string::npos);
    CHECK(trace.find("member-finished") != std::string::npos);
    CHECK(trace.find("phase-completed") != std::string::npos);
    auto const prompt = readFile(phase / "members" / "member-a" / "r1" / "prompt.md");
    CHECK(prompt.find("Depth: challenge") != std::string::npos);
    CHECK(prompt.find("Round: 1 of 2 (independent review)") != std::string::npos);
    CHECK(prompt.find("Depth contract: actively challenge assumptions") != std::string::npos);
    CHECK(prompt.find("Focus hints (advisory, not an enforcement boundary)") != std::string::npos);
    CHECK(prompt.find("git diff --stat refs/aobus-council/base..HEAD") != std::string::npos);
    auto const challengePrompt = readFile(phase / "members" / "member-a" / "r2" / "prompt.md");
    CHECK(challengePrompt.find("Round: 2 of 2 (cross-challenge)") != std::string::npos);
    CHECK(challengePrompt.find("chair for synthesis") != std::string::npos);
    CHECK(challengePrompt.find("--- member-b ---") != std::string::npos);
    CHECK(challengePrompt.find("--- member-a ---") == std::string::npos);

    auto const* memberA = memberRequest(fake, "member-a");
    REQUIRE(memberA != nullptr);
    CHECK(memberA->cwd == phase / "members" / "member-a" / "r1" / "workspace");
    CHECK(memberA->standardInput.find("Depth: challenge") != std::string::npos);
    REQUIRE(memberA->optStdoutSink);
    REQUIRE(memberA->optStderrSink);
    CHECK(memberA->optStdoutSink->path == phase / "members" / "member-a" / "r1" / "stdout.txt");
    CHECK(memberA->optStderrSink->path == phase / "members" / "member-a" / "r1" / "stderr.txt");
    CHECK(memberA->argv.front() == "bwrap");
    CHECK_FALSE(containsSequence(memberA->argv, {"--ro-bind", "/", "/"}));
    CHECK(containsSequence(
      memberA->argv,
      {"--bind", (phase / "members" / "member-a" / "r1" / "workspace").string(), (tempPath(temp) / "repo").string()}));
    CHECK(containsSequence(memberA->argv, {"--chdir", (tempPath(temp) / "repo").string(), "member-a"}));
  }

  TEST_CASE("Runner - panel depth runs one independent round", "[council][integration][engine]")
  {
    auto temp = ao::test::TempDir{};
    writeFile(temp, "repo/source.txt", "original\n");
    auto phase = intent("phase-a");
    phase.overrides.optDepth = Depth::Panel;
    phase.overrides.optRoster = std::vector<std::string>{"member-a", "member-b"};
    phase.overrides.optQuorum = 1;
    auto fake = FakeRunner{};
    auto run = Runner{fake}.run(registry(), {phase}, tempPath(temp) / "repo", tempPath(temp) / "out");

    REQUIRE(run);
    CHECK(run->manifests.front().failure == FailureReason::None);

    auto const phaseRoot = tempPath(temp) / "out" / "phase-a";
    CHECK(std::filesystem::exists(phaseRoot / "members" / "member-a" / "r1" / "prompt.md"));
    CHECK_FALSE(std::filesystem::exists(phaseRoot / "members" / "member-a" / "r2" / "prompt.md"));

    auto const prompt = readFile(phaseRoot / "members" / "member-a" / "r1" / "prompt.md");
    CHECK(prompt.find("Depth: panel") != std::string::npos);
    CHECK(prompt.find("Round: 1 of 1 (independent review)") != std::string::npos);
  }

  TEST_CASE("Runner - full depth adds self-revision after peer challenge", "[council][integration][engine]")
  {
    auto temp = ao::test::TempDir{};
    writeFile(temp, "repo/source.txt", "original\n");
    auto phase = intent("phase-a");
    phase.overrides.optDepth = Depth::Full;
    phase.overrides.optRoster = std::vector<std::string>{"member-a", "member-b"};
    phase.overrides.optQuorum = 2;
    auto fake = FakeRunner{};
    auto run = Runner{fake}.run(registry(), {phase}, tempPath(temp) / "repo", tempPath(temp) / "out");

    REQUIRE(run);
    CHECK(run->manifests.front().failure == FailureReason::None);

    auto const phaseRoot = tempPath(temp) / "out" / "phase-a";
    CHECK(std::filesystem::exists(phaseRoot / "members" / "member-a" / "r1" / "prompt.md"));
    CHECK(std::filesystem::exists(phaseRoot / "members" / "member-a" / "r2" / "prompt.md"));
    CHECK(std::filesystem::exists(phaseRoot / "members" / "member-a" / "r3" / "prompt.md"));

    auto const revisionPrompt = readFile(phaseRoot / "members" / "member-a" / "r3" / "prompt.md");
    CHECK(revisionPrompt.find("Depth: full") != std::string::npos);
    CHECK(revisionPrompt.find("Round: 3 of 3 (self-revision)") != std::string::npos);
    auto const challengePrompt = readFile(phaseRoot / "members" / "member-a" / "r2" / "prompt.md");
    CHECK(challengePrompt.find("draft authors for self-revision") != std::string::npos);
    CHECK(revisionPrompt.find("Your original review:") != std::string::npos);
    CHECK(revisionPrompt.find("Your own challenge notes:") != std::string::npos);
    CHECK(revisionPrompt.find("Revise your original review") != std::string::npos);
  }

  TEST_CASE("Runner - challenge depth fails when peer challenge misses quorum", "[council][integration][engine]")
  {
    auto temp = ao::test::TempDir{};
    writeFile(temp, "repo/source.txt", "original\n");
    auto phase = intent("phase-a");
    phase.overrides.optDepth = Depth::Challenge;
    phase.overrides.optRoster = std::vector<std::string>{"member-a", "member-b"};
    phase.overrides.optQuorum = 2;
    auto fake = FakeRunner{};
    fake.roundAgentResults.emplace(std::pair{std::string{"member-b"}, std::string{"r2"}},
                                   ProcessResult{.status = ProcessStatus::Exited, .exitCode = 0});
    auto run = Runner{fake}.run(registry(), {phase}, tempPath(temp) / "repo", tempPath(temp) / "out");

    REQUIRE(run);
    REQUIRE(run->manifests.size() == 1);
    CHECK(run->manifests.front().failure == FailureReason::QuorumFailed);
    CHECK(run->manifests.front().summary == "1 of 2 council members returned usable responses");
    CHECK(run->failed);

    auto const phaseRoot = tempPath(temp) / "out" / "phase-a";
    CHECK(std::filesystem::exists(phaseRoot / "members" / "member-a" / "r2" / "prompt.md"));
    CHECK_FALSE(std::filesystem::exists(phaseRoot / "members" / "member-a" / "r3" / "prompt.md"));
  }

  TEST_CASE("Runner - roster override narrows member launch", "[council][integration][engine]")
  {
    auto temp = ao::test::TempDir{};
    writeFile(temp, "repo/source.txt", "original\n");
    auto phase = intent("phase-a");
    phase.overrides.optRoster = std::vector<std::string>{"member-a", "member-b"};
    phase.overrides.optQuorum = 2;
    auto fake = FakeRunner{};
    auto run = Runner{fake}.run(registry(), {phase}, tempPath(temp) / "repo", tempPath(temp) / "out");

    REQUIRE(run);
    CHECK(run->manifests.front().failure == FailureReason::None);
    CHECK(launchedAgent(fake, "member-a"));
    CHECK(launchedAgent(fake, "member-b"));
    CHECK_FALSE(launchedAgent(fake, "member-c"));
  }

  TEST_CASE("Runner - roster members launch concurrently", "[council][integration][engine]")
  {
    auto temp = ao::test::TempDir{};
    writeFile(temp, "repo/source.txt", "original\n");
    auto phase = intent("phase-a");
    phase.overrides.optRoster = std::vector<std::string>{"member-a", "member-b"};
    phase.overrides.optQuorum = 2;

    auto fake = FakeRunner{};
    fake.requiredConcurrentAgents = 2;
    auto run = Runner{fake}.run(registry(), {phase}, tempPath(temp) / "repo", tempPath(temp) / "out");

    REQUIRE(run);
    REQUIRE(run->manifests.size() == 1);
    CHECK(run->manifests.front().failure == FailureReason::None);
    CHECK(fake.maxActiveAgents >= 2);
  }

  TEST_CASE("Runner - failed quorum skips dependent phases", "[council][integration][engine]")
  {
    auto temp = ao::test::TempDir{};
    writeFile(temp, "repo/source.txt", "original\n");
    auto first = intent("phase-a");
    first.overrides.optRoster = std::vector<std::string>{"member-a", "member-b"};
    first.overrides.optQuorum = 2;
    auto second = intent("phase-b");
    second.dependsOn = {"phase-a"};
    second.overrides.optRoster = std::vector<std::string>{"member-a", "member-b"};
    second.overrides.optQuorum = 2;

    auto fake = FakeRunner{};
    fake.agentResults.emplace("member-b", ProcessResult{.status = ProcessStatus::Exited, .exitCode = 7});
    auto run = Runner{fake}.run(registry(), {first, second}, tempPath(temp) / "repo", tempPath(temp) / "out");

    REQUIRE(run);
    REQUIRE(run->manifests.size() == 2);
    CHECK(run->manifests[0].failure == FailureReason::QuorumFailed);
    CHECK(run->manifests[1].failure == FailureReason::DependencyFailed);
    CHECK(run->failed);

    auto const skippedRoot = tempPath(temp) / "out" / "phase-b";
    CHECK(std::filesystem::exists(skippedRoot / "intent.yaml"));
    CHECK(std::filesystem::exists(skippedRoot / "resolved.yaml"));
    CHECK(std::filesystem::exists(skippedRoot / "evidence.yaml"));
    CHECK(std::filesystem::exists(skippedRoot / "dossier.md"));
    auto const skippedEvidence = readFile(skippedRoot / "evidence.yaml");
    CHECK(skippedEvidence.find("rounds: []") != std::string::npos);
    auto const skippedTrace = readFile(skippedRoot / "trace.yaml");
    CHECK(skippedTrace.find("phase-started") != std::string::npos);
    CHECK(skippedTrace.find("phase-completed") != std::string::npos);
  }

  TEST_CASE("Runner - empty member output does not satisfy quorum", "[council][integration][engine]")
  {
    auto temp = ao::test::TempDir{};
    writeFile(temp, "repo/source.txt", "original\n");
    auto phase = intent("phase-a");
    phase.overrides.optRoster = std::vector<std::string>{"member-a", "member-b"};
    phase.overrides.optQuorum = 2;

    auto fake = FakeRunner{};
    fake.agentResults.emplace("member-b", ProcessResult{.status = ProcessStatus::Exited, .exitCode = 0});
    auto run = Runner{fake}.run(registry(), {phase}, tempPath(temp) / "repo", tempPath(temp) / "out");

    REQUIRE(run);
    REQUIRE(run->manifests.size() == 1);
    CHECK(run->manifests.front().failure == FailureReason::QuorumFailed);
    CHECK(run->manifests.front().summary == "1 of 2 council members returned usable responses");
    CHECK(run->failed);
  }

  TEST_CASE("Runner - authentication prompts do not satisfy quorum", "[council][integration][engine]")
  {
    auto temp = ao::test::TempDir{};
    writeFile(temp, "repo/source.txt", "original\n");
    auto phase = intent("phase-a");
    phase.overrides.optDepth = Depth::Panel;
    phase.overrides.optRoster = std::vector<std::string>{"member-a"};
    phase.overrides.optQuorum = 1;

    auto fake = FakeRunner{};
    fake.agentResults.emplace(
      "member-a",
      ProcessResult{.status = ProcessStatus::Exited,
                    .exitCode = 0,
                    .standardOutput = "Authentication required. Waiting for authentication.\n"});
    auto run = Runner{fake}.run(registry(), {phase}, tempPath(temp) / "repo", tempPath(temp) / "out");

    REQUIRE(run);
    REQUIRE(run->manifests.size() == 1);
    CHECK(run->manifests.front().failure == FailureReason::QuorumFailed);
    CHECK(run->manifests.front().summary == "0 of 1 council members returned usable responses");
    CHECK(run->failed);

    auto const evidence = readFile(tempPath(temp) / "out" / "phase-a" / "evidence.yaml");
    CHECK(evidence.find("usable: false") != std::string::npos);
  }

  TEST_CASE("Runner - auth prompts on either stream do not satisfy quorum", "[council][integration][engine]")
  {
    auto temp = ao::test::TempDir{};
    writeFile(temp, "repo/source.txt", "original\n");
    auto phase = intent("phase-a");
    phase.overrides.optDepth = Depth::Panel;
    phase.overrides.optRoster = std::vector<std::string>{"member-a"};
    phase.overrides.optQuorum = 1;

    auto fake = FakeRunner{};
    fake.agentResults.emplace("member-a",
                              ProcessResult{.status = ProcessStatus::Exited,
                                            .exitCode = 0,
                                            .standardOutput = "Tool startup complete.\n",
                                            .standardError = "Login required. Please run /login.\n"});
    auto run = Runner{fake}.run(registry(), {phase}, tempPath(temp) / "repo", tempPath(temp) / "out");

    REQUIRE(run);
    REQUIRE(run->manifests.size() == 1);
    CHECK(run->manifests.front().failure == FailureReason::QuorumFailed);
    CHECK(run->manifests.front().summary == "0 of 1 council members returned usable responses");
    CHECK(run->failed);

    auto const evidence = readFile(tempPath(temp) / "out" / "phase-a" / "evidence.yaml");
    CHECK(evidence.find("usable: false") != std::string::npos);
  }

  TEST_CASE("Runner - stderr-only successful reviews satisfy quorum", "[council][integration][engine]")
  {
    auto temp = ao::test::TempDir{};
    writeFile(temp, "repo/source.txt", "original\n");
    auto phase = intent("phase-a");
    phase.overrides.optDepth = Depth::Panel;
    phase.overrides.optRoster = std::vector<std::string>{"member-a"};
    phase.overrides.optQuorum = 1;

    auto fake = FakeRunner{};
    fake.agentResults.emplace("member-a",
                              ProcessResult{.status = ProcessStatus::Exited,
                                            .exitCode = 0,
                                            .standardError = "Finding: stderr carries the final review.\n"});
    auto run = Runner{fake}.run(registry(), {phase}, tempPath(temp) / "repo", tempPath(temp) / "out");

    REQUIRE(run);
    REQUIRE(run->manifests.size() == 1);
    CHECK(run->manifests.front().failure == FailureReason::None);

    auto const evidence = readFile(tempPath(temp) / "out" / "phase-a" / "evidence.yaml");
    CHECK(evidence.find("usable: true") != std::string::npos);
    CHECK(evidence.find("review-stream: stderr") != std::string::npos);
  }

  TEST_CASE("Runner - auth-related review text is not treated as an auth prompt", "[council][integration][engine]")
  {
    auto temp = ao::test::TempDir{};
    writeFile(temp, "repo/source.txt", "original\n");
    auto phase = intent("phase-a");
    phase.overrides.optDepth = Depth::Panel;
    phase.overrides.optRoster = std::vector<std::string>{"member-a"};
    phase.overrides.optQuorum = 1;

    auto fake = FakeRunner{};
    fake.agentResults.emplace(
      "member-a",
      ProcessResult{.status = ProcessStatus::Exited,
                    .exitCode = 0,
                    .standardOutput = "Review finding: OAuth authentication required flows are documented.\n"});
    auto run = Runner{fake}.run(registry(), {phase}, tempPath(temp) / "repo", tempPath(temp) / "out");

    REQUIRE(run);
    REQUIRE(run->manifests.size() == 1);
    CHECK(run->manifests.front().failure == FailureReason::None);
  }

  TEST_CASE("Runner - long reviews containing auth prompt examples are usable", "[council][integration][engine]")
  {
    auto temp = ao::test::TempDir{};
    writeFile(temp, "repo/source.txt", "original\n");
    auto phase = intent("phase-a");
    phase.overrides.optDepth = Depth::Panel;
    phase.overrides.optRoster = std::vector<std::string>{"member-a"};
    phase.overrides.optQuorum = 1;

    auto review = std::string{"Review finding: tests contain the literal text Waiting for authentication.\n"};
    review += std::string(5000, 'x');
    auto fake = FakeRunner{};
    fake.agentResults.emplace(
      "member-a", ProcessResult{.status = ProcessStatus::Exited, .exitCode = 0, .standardOutput = std::move(review)});
    auto run = Runner{fake}.run(registry(), {phase}, tempPath(temp) / "repo", tempPath(temp) / "out");

    REQUIRE(run);
    REQUIRE(run->manifests.size() == 1);
    CHECK(run->manifests.front().failure == FailureReason::None);
  }

  TEST_CASE("Runner - real repository mutation is reported as a conflict", "[council][integration][engine]")
  {
    auto temp = ao::test::TempDir{};
    auto const source = writeFile(temp, "repo/source.txt", "original\n");
    auto fake = FakeRunner{};
    fake.mutateRealRepoPath = source;

    auto run = Runner{fake}.run(registry(), {intent("phase-a")}, tempPath(temp) / "repo", tempPath(temp) / "out");

    REQUIRE_FALSE(run);
    CHECK(run.error().code == Error::Code::Conflict);
    CHECK(run.error().message.find("real repository changed") != std::string::npos);

    auto const manifest = readFile(tempPath(temp) / "out" / "phase-a" / "manifest.yaml");
    CHECK(manifest.find("failure: real-tree-changed") != std::string::npos);
    CHECK(manifest.find("delegated results were discarded") != std::string::npos);
    auto const trace = readFile(tempPath(temp) / "out" / "phase-a" / "trace.yaml");
    CHECK(trace.find("run-discarded") != std::string::npos);
  }

  TEST_CASE("Runner - sandbox launch failures are infrastructure errors", "[council][integration][engine]")
  {
    auto temp = ao::test::TempDir{};
    writeFile(temp, "repo/source.txt", "original\n");
    auto phase = intent("phase-a");
    phase.overrides.optDepth = Depth::Panel;
    phase.overrides.optRoster = std::vector<std::string>{"member-a"};
    phase.overrides.optQuorum = 1;

    auto fake = FakeRunner{};
    fake.agentResults.emplace(
      "member-a", ProcessResult{.status = ProcessStatus::LaunchFailed, .standardError = "executable not found: bwrap"});
    auto run = Runner{fake}.run(registry(), {phase}, tempPath(temp) / "repo", tempPath(temp) / "out");

    REQUIRE(run);
    REQUIRE(run->manifests.size() == 1);
    CHECK(run->manifests.front().failure == FailureReason::InfrastructureFailed);
    CHECK(run->manifests.front().summary.find("sandbox launch failed") != std::string::npos);
    CHECK(run->failed);

    auto const phaseRoot = tempPath(temp) / "out" / "phase-a";
    CHECK(std::filesystem::exists(phaseRoot / "evidence.yaml"));
    CHECK(std::filesystem::exists(phaseRoot / "dossier.md"));
    auto const manifest = readFile(phaseRoot / "manifest.yaml");
    CHECK(manifest.find("failure: infrastructure-failed") != std::string::npos);
  }

  TEST_CASE("Runner - infrastructure failures skip dependent phases but keep scheduling",
            "[council][integration][engine]")
  {
    auto temp = ao::test::TempDir{};
    writeFile(temp, "repo/source.txt", "original\n");
    auto first = intent("phase-a");
    first.overrides.optDepth = Depth::Panel;
    first.overrides.optRoster = std::vector<std::string>{"member-a"};
    first.overrides.optQuorum = 1;
    auto second = intent("phase-b");
    second.dependsOn = {"phase-a"};
    second.overrides.optDepth = Depth::Panel;
    second.overrides.optRoster = std::vector<std::string>{"member-b"};
    second.overrides.optQuorum = 1;
    auto third = intent("phase-c");
    third.overrides.optDepth = Depth::Panel;
    third.overrides.optRoster = std::vector<std::string>{"member-c"};
    third.overrides.optQuorum = 1;

    auto fake = FakeRunner{};
    fake.agentResults.emplace(
      "member-a", ProcessResult{.status = ProcessStatus::LaunchFailed, .standardError = "executable not found: bwrap"});
    auto run = Runner{fake}.run(registry(), {first, second, third}, tempPath(temp) / "repo", tempPath(temp) / "out");

    REQUIRE(run);
    REQUIRE(run->manifests.size() == 3);
    CHECK(run->manifests[0].failure == FailureReason::InfrastructureFailed);
    CHECK(run->manifests[1].failure == FailureReason::DependencyFailed);
    CHECK(run->manifests[2].failure == FailureReason::None);
    CHECK(run->failed);
    CHECK_FALSE(launchedAgent(fake, "member-b"));
    CHECK(launchedAgent(fake, "member-c"));
    CHECK(std::filesystem::exists(tempPath(temp) / "out" / "phase-b" / "dossier.md"));
  }

  TEST_CASE("Runner - member infrastructure failures are tolerated when quorum is met",
            "[council][integration][engine]")
  {
    auto temp = ao::test::TempDir{};
    writeFile(temp, "repo/source.txt", "original\n");
    auto phase = intent("phase-a");
    phase.overrides.optDepth = Depth::Panel;
    phase.overrides.optRoster = std::vector<std::string>{"member-a", "member-b", "member-c"};
    phase.overrides.optQuorum = 2;

    auto fake = FakeRunner{};
    fake.agentResults.emplace(
      "member-c", ProcessResult{.status = ProcessStatus::LaunchFailed, .standardError = "executable not found: bwrap"});
    auto run = Runner{fake}.run(registry(), {phase}, tempPath(temp) / "repo", tempPath(temp) / "out");

    REQUIRE(run);
    REQUIRE(run->manifests.size() == 1);
    CHECK(run->manifests.front().failure == FailureReason::None);
    CHECK_FALSE(run->failed);

    auto const evidence = readFile(tempPath(temp) / "out" / "phase-a" / "evidence.yaml");
    CHECK(evidence.find("agent: member-c") != std::string::npos);
    CHECK(evidence.find("status: launch-failed") != std::string::npos);
    CHECK(evidence.find("usable: false") != std::string::npos);
  }

  TEST_CASE("Runner - prompt delivery modes are reflected in member requests", "[council][integration][engine]")
  {
    auto temp = ao::test::TempDir{};
    writeFile(temp, "repo/source.txt", "original\n");
    auto phase = intent("phase-a");
    phase.overrides.optDepth = Depth::Panel;
    phase.overrides.optRoster = std::vector<std::string>{"member-a"};
    phase.overrides.optQuorum = 1;

    SECTION("argument delivery appends the prompt to argv")
    {
      auto reg = registry();
      reg.agents.at("member-a").promptDelivery = PromptDelivery::Argument;
      auto fake = FakeRunner{};
      auto run = Runner{fake}.run(reg, {phase}, tempPath(temp) / "repo", tempPath(temp) / "out");

      REQUIRE(run);
      auto const* memberA = memberRequest(fake, "member-a");
      REQUIRE(memberA != nullptr);
      CHECK(memberA->standardInput.empty());
      CHECK(std::ranges::any_of(
        memberA->argv,
        [](std::string const& value)
        { return value.find("You are one member of an Aobus council review.") != std::string::npos; }));
    }

    SECTION("file delivery appends the prompt path to argv")
    {
      auto reg = registry();
      reg.agents.at("member-a").promptDelivery = PromptDelivery::File;
      auto fake = FakeRunner{};
      auto run = Runner{fake}.run(reg, {phase}, tempPath(temp) / "repo", tempPath(temp) / "out");

      REQUIRE(run);
      auto const* memberA = memberRequest(fake, "member-a");
      REQUIRE(memberA != nullptr);
      CHECK(memberA->standardInput.empty());
      CHECK(
        std::ranges::contains(memberA->argv, (tempPath(temp) / "repo" / ".git" / "aobus-council-prompt.md").string()));
      CHECK(readFile(tempPath(temp) / "out" / "phase-a" / "members" / "member-a" / "r1" / "workspace" / ".git" /
                     "aobus-council-prompt.md")
              .find("You are one member of an Aobus council review.") != std::string::npos);
      CHECK_FALSE(std::filesystem::exists(tempPath(temp) / "out" / "phase-a" / "members" / "member-a" / "r1" /
                                          "workspace" / ".aobus-council-prompt.md"));
    }

    SECTION("file delivery replaces prompt-file placeholders")
    {
      auto reg = registry();
      reg.agents.at("member-a").argvTemplate = {"member-a", "--prompt-file", "{prompt-file}"};
      reg.agents.at("member-a").promptDelivery = PromptDelivery::File;
      auto fake = FakeRunner{};
      auto run = Runner{fake}.run(reg, {phase}, tempPath(temp) / "repo", tempPath(temp) / "out");

      REQUIRE(run);
      auto const* memberA = memberRequest(fake, "member-a");
      REQUIRE(memberA != nullptr);
      auto const promptPath = (tempPath(temp) / "repo" / ".git" / "aobus-council-prompt.md").string();
      CHECK(containsSequence(memberA->argv, {"member-a", "--prompt-file", promptPath}));
      CHECK(std::ranges::count(memberA->argv, promptPath) == 1);
      CHECK(readFile(tempPath(temp) / "out" / "phase-a" / "members" / "member-a" / "r1" / "workspace" / ".git" /
                     "aobus-council-prompt.md")
              .find("You are one member of an Aobus council review.") != std::string::npos);
    }

    SECTION("runtime placeholder replacement does not rescan prompt text")
    {
      auto reg = registry();
      reg.agents.at("member-a").argvTemplate = {"member-a", "--prompt", "{prompt}"};
      reg.agents.at("member-a").promptDelivery = PromptDelivery::Argument;
      phase.body = "Review the literal {prompt-file} marker.";
      auto fake = FakeRunner{};
      auto run = Runner{fake}.run(reg, {phase}, tempPath(temp) / "repo", tempPath(temp) / "out");

      REQUIRE(run);
      auto const* memberA = memberRequest(fake, "member-a");
      REQUIRE(memberA != nullptr);
      auto const promptPath = (tempPath(temp) / "repo" / ".git" / "aobus-council-prompt.md").string();
      CHECK_FALSE(std::ranges::contains(memberA->argv, promptPath));
      CHECK(std::ranges::any_of(memberA->argv,
                                [](std::string const& value)
                                { return value.find("literal {prompt-file} marker") != std::string::npos; }));
    }
  }

  TEST_CASE("Runner - immutable base is reclaimed after the run", "[council][integration][engine]")
  {
    auto temp = ao::test::TempDir{};
    writeFile(temp, "repo/source.txt", "original\n");
    auto fake = FakeRunner{};

    auto run = Runner{fake}.run(registry(), {intent("phase-a")}, tempPath(temp) / "repo", tempPath(temp) / "out");

    REQUIRE(run);
    // The base is the rsync source for every member workspace; nothing inspects it afterwards, so teardown
    // must reclaim it rather than leave a full repo copy behind.
    CHECK_FALSE(std::filesystem::exists(tempPath(temp) / "out" / ".base"));
  }

  TEST_CASE("Runner - output path inside the repository is rejected", "[council][integration][engine]")
  {
    auto temp = ao::test::TempDir{};
    writeFile(temp, "repo/source.txt", "original\n");
    auto fake = FakeRunner{};
    auto run = Runner{fake}.run(registry(), {intent("phase-a")}, tempPath(temp) / "repo", tempPath(temp) / "repo/out");

    REQUIRE_FALSE(run);
    CHECK(run.error().code == Error::Code::InvalidState);
    CHECK(run.error().message.find("must be outside repository") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(tempPath(temp) / "repo/out"));
  }

  TEST_CASE("Runner - relative repo and output paths are normalized before sandboxing",
            "[council][integration][engine]")
  {
    auto temp = ao::test::TempDir{};
    writeFile(temp, "repo/source.txt", "original\n");
    auto const guard = CurrentPathGuard{tempPath(temp)};
    auto fake = FakeRunner{};

    auto run = Runner{fake}.run(registry(), {intent("phase-a")}, "repo", "out");

    REQUIRE(run);
    auto const normalizedRepo = std::filesystem::weakly_canonical(tempPath(temp) / "repo");
    auto const normalizedOut = std::filesystem::weakly_canonical(tempPath(temp) / "out");
    auto const phaseRoot = normalizedOut / "phase-a";
    auto const* memberA = memberRequest(fake, "member-a");
    REQUIRE(memberA != nullptr);
    CHECK(memberA->cwd == phaseRoot / "members" / "member-a" / "r1" / "workspace");
    CHECK(containsSequence(
      memberA->argv,
      {"--bind", (phaseRoot / "members" / "member-a" / "r1" / "workspace").string(), normalizedRepo.string()}));
    CHECK(containsSequence(memberA->argv, {"--chdir", normalizedRepo.string(), "member-a"}));
  }

  TEST_CASE("Runner - invalid resolved intents are rejected before output setup", "[council][integration][engine]")
  {
    auto temp = ao::test::TempDir{};
    writeFile(temp, "repo/source.txt", "original\n");
    auto first = intent("phase-a");
    auto second = intent("phase-b");
    second.taskKind = "missing-council";
    auto fake = FakeRunner{};

    auto run = Runner{fake}.run(registry(), {first, second}, tempPath(temp) / "repo", tempPath(temp) / "out");

    REQUIRE_FALSE(run);
    CHECK(run.error().code == Error::Code::InvalidInput);
    CHECK(run.error().message.find("no definition for task-kind 'missing-council'") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(tempPath(temp) / "out"));
    CHECK(fake.requests.empty());
  }

  TEST_CASE("Runner - output path must be fresh", "[council][integration][engine]")
  {
    auto temp = ao::test::TempDir{};
    writeFile(temp, "repo/source.txt", "original\n");
    writeFile(temp, "out/stale.txt", "old run\n");
    auto fake = FakeRunner{};

    auto run = Runner{fake}.run(registry(), {intent("phase-a")}, tempPath(temp) / "repo", tempPath(temp) / "out");

    REQUIRE_FALSE(run);
    CHECK(run.error().code == Error::Code::InvalidState);
    CHECK(run.error().message.find("must be empty") != std::string::npos);
  }

  TEST_CASE("Runner - output path symlinks are rejected", "[council][integration][engine]")
  {
    auto temp = ao::test::TempDir{};
    writeFile(temp, "repo/source.txt", "original\n");
    std::filesystem::create_directories(tempPath(temp) / "actual-out");
    std::filesystem::create_symlink(tempPath(temp) / "actual-out", tempPath(temp) / "out-link");
    auto fake = FakeRunner{};

    auto run = Runner{fake}.run(registry(), {intent("phase-a")}, tempPath(temp) / "repo", tempPath(temp) / "out-link");

    REQUIRE_FALSE(run);
    CHECK(run.error().code == Error::Code::InvalidState);
    CHECK(run.error().message.find("fresh directory") != std::string::npos);
  }
} // namespace ao::council::test
