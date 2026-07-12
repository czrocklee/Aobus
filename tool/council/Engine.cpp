// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "council/Engine.h"

#include "council/CouncilSchema.h"
#include "council/ProcessRunner.h"
#include "council/Serialization.h"
#include "council/Substrate.h"
#include "council/YamlEmit.h"
#include <ao/Error.h>
#include <ao/async/ImmediateExecutor.h>
#include <ao/async/Runtime.h>
#include <ao/async/Task.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <future>
#include <initializer_list>
#include <iterator>
#include <map>
#include <optional>
#include <print>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace ao::council
{
  namespace
  {
    constexpr auto kWorkspacePromptFile = std::string_view{".git/aobus-council-prompt.md"};

    template<typename Callback>
    class ScopeExit final
    {
    public:
      explicit ScopeExit(Callback callback)
        : _callback{std::move(callback)}
      {
      }

      ScopeExit(ScopeExit const&) = delete;
      ScopeExit(ScopeExit&&) = delete;
      ScopeExit& operator=(ScopeExit const&) = delete;
      ScopeExit& operator=(ScopeExit&&) = delete;

      ~ScopeExit() { _callback(); }

    private:
      Callback _callback;
    };

    Result<> writeArtifact(ArtifactStore const& store, std::filesystem::path const& path, std::string_view content)
    {
      if (auto result = store.write(path, content); !result)
      {
        return std::unexpected{result.error()};
      }

      return {};
    }

    Result<> appendTrace(ArtifactStore const& store,
                         std::string_view event,
                         std::map<std::string, std::string, std::less<>> fields)
    {
      if (auto result = store.append("trace.yaml", emitTraceEvent(event, fields)); !result)
      {
        return std::unexpected{result.error()};
      }

      return {};
    }

    Result<std::filesystem::path> normalizedAbsolute(std::filesystem::path const& path)
    {
      auto error = std::error_code{};
      auto result = std::filesystem::weakly_canonical(path, error);

      if (!error)
      {
        return result.lexically_normal();
      }

      error.clear();
      result = std::filesystem::absolute(path, error);

      if (error)
      {
        return makeError(Error::Code::IoError, error.message());
      }

      return result.lexically_normal();
    }

    bool isSameOrDescendant(std::filesystem::path const& child, std::filesystem::path const& parent)
    {
      auto childIt = child.begin();

      for (auto const& element : parent)
      {
        if (childIt == child.end() || *childIt != element)
        {
          return false;
        }

        ++childIt;
      }

      return true;
    }

    Result<> validateOutputPath(std::filesystem::path const& repo, std::filesystem::path const& out)
    {
      auto normalizedRepo = normalizedAbsolute(repo);

      if (!normalizedRepo)
      {
        return std::unexpected{normalizedRepo.error()};
      }

      auto normalizedOut = normalizedAbsolute(out);

      if (!normalizedOut)
      {
        return std::unexpected{normalizedOut.error()};
      }

      if (isSameOrDescendant(*normalizedOut, *normalizedRepo))
      {
        return makeError(Error::Code::InvalidState,
                         std::format("council output path '{}' must be outside repository '{}'",
                                     normalizedOut->string(),
                                     normalizedRepo->string()));
      }

      return {};
    }

    Result<> prepareFreshOutputRoot(std::filesystem::path const& out)
    {
      auto error = std::error_code{};
      auto const status = std::filesystem::symlink_status(out, error);

      if (error && error != std::errc::no_such_file_or_directory)
      {
        return makeError(Error::Code::IoError, error.message());
      }

      if (!error && std::filesystem::exists(status))
      {
        if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status))
        {
          return makeError(
            Error::Code::InvalidState, std::format("council output path '{}' must be a fresh directory", out.string()));
        }

        if (!std::filesystem::is_empty(out, error))
        {
          if (error)
          {
            return makeError(Error::Code::IoError, error.message());
          }

          return makeError(Error::Code::InvalidState,
                           std::format("council output path '{}' must be empty before a run", out.string()));
        }

        return {};
      }

      std::filesystem::create_directories(out, error);

      if (error)
      {
        return makeError(Error::Code::IoError, error.message());
      }

      return {};
    }

    struct RunPaths final
    {
      std::filesystem::path repo = {};
      std::filesystem::path out = {};
    };

    Result<RunPaths> prepareRunPaths(std::filesystem::path const& repo, std::filesystem::path const& out)
    {
      if (auto result = validateOutputPath(repo, out); !result)
      {
        return std::unexpected{result.error()};
      }

      if (auto result = prepareFreshOutputRoot(out); !result)
      {
        return std::unexpected{result.error()};
      }

      auto normalizedRepo = normalizedAbsolute(repo);

      if (!normalizedRepo)
      {
        return std::unexpected{normalizedRepo.error()};
      }

      auto normalizedOut = normalizedAbsolute(out);

      if (!normalizedOut)
      {
        return std::unexpected{normalizedOut.error()};
      }

      return RunPaths{.repo = std::move(*normalizedRepo), .out = std::move(*normalizedOut)};
    }

    bool hasNonWhitespace(std::string_view value)
    {
      return std::ranges::any_of(
        value,
        [](char character) { return character != ' ' && character != '\n' && character != '\r' && character != '\t'; });
    }

    std::string asciiLower(std::string_view value)
    {
      auto result = std::string{};
      result.reserve(value.size());

      for (auto const character : value)
      {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
      }

      return result;
    }

    bool containsAny(std::string_view haystack, std::initializer_list<std::string_view> needles)
    {
      return std::ranges::any_of(needles, [&](std::string_view needle) { return haystack.contains(needle); });
    }

    std::string trimAsciiWhitespace(std::string value)
    {
      auto const first = value.find_first_not_of(" \n\r\t");

      if (first == std::string::npos)
      {
        return {};
      }

      auto const last = value.find_last_not_of(" \n\r\t");
      return value.substr(first, last - first + 1);
    }

    std::string_view memberReviewText(ProcessResult const& result)
    {
      if (hasNonWhitespace(result.standardOutput))
      {
        return result.standardOutput;
      }

      return result.standardError;
    }

    std::string_view memberReviewStream(ProcessResult const& result)
    {
      return hasNonWhitespace(result.standardOutput) ? std::string_view{"stdout"} : std::string_view{"stderr"};
    }

    bool looksLikeAuthenticationChallenge(ProcessResult const& result)
    {
      auto const hasChallenge = [](std::string_view text)
      {
        if (!hasNonWhitespace(text))
        {
          return false;
        }

        auto const normalized = trimAsciiWhitespace(asciiLower(std::string{text}));
        constexpr std::size_t kMaxChallengeTextSize = 4096;

        if (normalized.size() > kMaxChallengeTextSize)
        {
          return false;
        }

        return containsAny(normalized,
                           {"authentication timed out",
                            "please visit the url to log in",
                            "not logged in",
                            "please run /login",
                            "login required",
                            "waiting for authentication"});
      };

      return hasChallenge(result.standardOutput) || hasChallenge(result.standardError);
    }

    bool isUsableMemberResponse(ProcessResult const& result)
    {
      return result.status == ProcessStatus::Exited && result.exitCode == 0 &&
             hasNonWhitespace(memberReviewText(result)) && !looksLikeAuthenticationChallenge(result);
    }

    std::string focusPrompt(std::vector<FocusRule> const& focus)
    {
      if (focus.empty())
      {
        return "Focus hints: entire repository; use judgment.\n";
      }

      auto out = std::ostringstream{};
      std::print(out, "Focus hints (advisory, not an enforcement boundary):\n");

      for (auto const& rule : focus)
      {
        std::print(out, "- {}", rule.path.generic_string());

        if (rule.match == FocusMatch::Prefix)
        {
          std::print(out, "/");
        }

        std::print(out, "\n");
      }

      return out.str();
    }

    std::string promptFor(ResolvedPhase const& phase,
                          AgentDefinition const& agent,
                          std::string_view roundLabel,
                          std::string_view roundContext)
    {
      auto out = std::ostringstream{};
      std::print(out, "You are one member of an Aobus council review.\n\n");
      std::println(out, "Phase: {}", phase.intent.id);
      std::println(out, "Task kind: {}", phase.intent.taskKind);
      std::println(out, "Model identity: {} ({})", agent.id, agent.modelVersion());
      std::println(out, "Depth: {}", toString(phase.definition.parameters.depth));
      std::println(out, "Round: {}", roundLabel);
      std::print(
        out, "Quorum: {} of {}\n\n", phase.definition.parameters.quorum, phase.definition.parameters.roster.size());
      std::print(out, "Review target:\n");
      std::print(out, "- The workspace is a sealed copy of the real pre-run working tree.\n");
      std::print(
        out,
        "- When `{}` exists, review uncommitted changes with `git diff --stat {}..HEAD` and `git diff {}..HEAD`.\n",
        kReviewBaseRef,
        kReviewBaseRef,
        kReviewBaseRef);
      std::print(out, "- If that ref is absent, inspect the current workspace HEAD and the requested focus hints.\n\n");
      std::print(out, "Invariant:\n{}\n\n", phase.intent.invariant);
      std::print(out, "{}\n", focusPrompt(phase.intent.focus));
      std::print(out, "Task:\n{}\n\n", phase.intent.body);

      switch (phase.definition.parameters.depth)
      {
        case Depth::Panel:
          std::print(out, "Depth contract: give a concise independent review focused on the highest-value findings.\n");
          break;
        case Depth::Challenge:
          std::print(out,
                     "Depth contract: actively challenge assumptions, look for regressions, and call out missing "
                     "validation.\n");
          break;
        case Depth::Full:
          std::print(out,
                     "Depth contract: provide a full analysis with alternatives, risks, validation strategy, and open "
                     "questions.\n");
          break;
      }

      std::print(out, "Do not edit the real repository.\n");

      if (!roundContext.empty())
      {
        std::print(out, "\nRound context:\n{}\n", roundContext);
      }

      return out.str();
    }

    std::string replaceRuntimePlaceholders(std::string_view value,
                                           std::string_view prompt,
                                           std::string_view promptPath,
                                           bool& consumedPrompt)
    {
      auto result = std::string{};
      result.reserve(value.size());

      for (std::size_t position = 0; position < value.size();)
      {
        if (value.substr(position).starts_with("{prompt-file}"))
        {
          result += promptPath;
          position += std::string_view{"{prompt-file}"}.size();
          consumedPrompt = true;
          continue;
        }

        if (value.substr(position).starts_with("{prompt}"))
        {
          result += prompt;
          position += std::string_view{"{prompt}"}.size();
          consumedPrompt = true;
          continue;
        }

        result.push_back(value[position]);
        ++position;
      }

      return result;
    }

    ProcessRequest requestFor(AgentDefinition const& agent,
                              std::filesystem::path const& workspace,
                              std::filesystem::path const& promptPath,
                              std::string const& prompt,
                              std::filesystem::path const& stdoutPath,
                              std::filesystem::path const& stderrPath)
    {
      auto argv = agent.argvTemplate;
      bool consumedPrompt = false;
      auto const promptPathText = promptPath.string();

      for (auto& argument : argv)
      {
        argument = replaceRuntimePlaceholders(argument, prompt, promptPathText, consumedPrompt);
      }

      auto request = ProcessRequest{
        .argv = std::move(argv),
        .cwd = workspace,
        .standardInput = {},
        .environmentWhitelist = agent.environmentWhitelist,
        .environment = {},
        .timeout = agent.timeout,
        .terminationGracePeriod = std::chrono::seconds{2},
        .optStdoutSink = StreamSink{.path = stdoutPath},
        .optStderrSink = StreamSink{.path = stderrPath},
      };

      if (agent.promptDelivery == PromptDelivery::Stdin)
      {
        request.standardInput = prompt;
      }
      else if (!consumedPrompt && agent.promptDelivery == PromptDelivery::Argument)
      {
        request.argv.push_back(prompt);
      }
      else if (!consumedPrompt && agent.promptDelivery == PromptDelivery::File)
      {
        request.argv.push_back(promptPath.string());
      }

      return request;
    }

    std::string processSummary(ProcessResult const& result)
    {
      return std::format("{} exit={} signal={}", toString(result.status), result.exitCode, result.signal);
    }

    std::string memberResponse(ProcessResult const& result)
    {
      auto out = std::ostringstream{};
      std::println(out, "status: {}", toString(result.status));
      std::print(out, "exit-code: {}\n\n", result.exitCode);
      std::println(out, "usable: {}", isUsableMemberResponse(result) ? "true" : "false");
      std::println(out, "review-stream: {}", memberReviewStream(result));
      std::println(out, "stdout-truncated: {}", result.standardOutputTruncated ? "true" : "false");
      std::print(out, "stderr-truncated: {}\n\n", result.standardErrorTruncated ? "true" : "false");
      std::print(out, "stdout:\n{}\n", result.standardOutput);

      if (!result.standardError.empty())
      {
        std::print(out, "\nstderr:\n{}\n", result.standardError);
      }

      return out.str();
    }

    struct MemberRoundResult final
    {
      std::string agentId = {};
      std::string roundId = {};
      std::string roundLabel = {};
      ProcessResult processResult = {};
    };

    using CouncilRows = std::vector<std::pair<std::string, std::string>>;
    using CouncilContexts = std::map<std::string, std::string, std::less<>>;

    std::vector<std::string> roundOrder(std::vector<MemberRoundResult> const& memberResults)
    {
      auto result = std::vector<std::string>{};

      for (auto const& member : memberResults)
      {
        if (!std::ranges::contains(result, member.roundId))
        {
          result.push_back(member.roundId);
        }
      }

      return result;
    }

    std::string roundLabel(std::vector<MemberRoundResult> const& memberResults, std::string const& roundId)
    {
      for (auto const& member : memberResults)
      {
        if (member.roundId == roundId)
        {
          return member.roundLabel;
        }
      }

      return roundId;
    }

    std::string dossierFor(ResolvedPhase const& phase, std::vector<MemberRoundResult> const& memberResults)
    {
      auto out = std::ostringstream{};
      std::print(out, "# Dossier\n\n");
      std::println(out, "Phase: {}", phase.intent.id);
      std::println(out, "Task kind: {}", phase.intent.taskKind);
      std::println(out, "Depth: {}", toString(phase.definition.parameters.depth));
      std::print(out, "Quorum: {}\n\n", phase.definition.parameters.quorum);
      std::print(out, "## Invariant\n\n{}\n\n", phase.intent.invariant);
      std::print(out, "## Focus\n\n{}\n", focusPrompt(phase.intent.focus));
      std::print(out, "## Body\n\n{}\n\n", phase.intent.body);

      for (auto const& roundId : roundOrder(memberResults))
      {
        std::println(out, "## {}", roundLabel(memberResults, roundId));

        for (auto const& member : memberResults)
        {
          if (member.roundId != roundId)
          {
            continue;
          }

          std::print(out, "\n### {}\n\n", member.agentId);
          std::print(out, "{}\n\n", processSummary(member.processResult));
          std::print(out, "usable: {}\n\n", isUsableMemberResponse(member.processResult) ? "true" : "false");
          std::print(out, "stdout:\n{}\n", member.processResult.standardOutput);

          if (!member.processResult.standardError.empty())
          {
            std::print(out, "\nstderr:\n{}\n", member.processResult.standardError);
          }
        }

        std::print(out, "\n");
      }

      return out.str();
    }

    std::string evidenceFor(ResolvedPhase const& phase, std::vector<MemberRoundResult> const& memberResults)
    {
      auto out = std::ostringstream{};
      yaml_emit::scalarField(out, 0, "schema", "aobus-council-evidence/v1");
      yaml_emit::scalarField(out, 0, "phase-id", phase.intent.id);

      if (memberResults.empty())
      {
        yaml_emit::emptySequenceField(out, 0, "rounds");
        return out.str();
      }

      yaml_emit::beginSequenceField(out, 0, "rounds");

      for (auto const& roundId : roundOrder(memberResults))
      {
        yaml_emit::beginSequenceMap(out, 2, "round", roundId);
        yaml_emit::scalarField(out, 4, "label", roundLabel(memberResults, roundId));
        yaml_emit::beginSequenceField(out, 4, "members");

        for (auto const& member : memberResults)
        {
          if (member.roundId != roundId)
          {
            continue;
          }

          constexpr std::size_t kMemberIndent = 6;
          auto const memberPath = std::filesystem::path{"members"} / member.agentId / member.roundId;
          yaml_emit::beginSequenceMap(out, kMemberIndent, "agent", member.agentId);
          yaml_emit::scalarField(out, 8, "status", toString(member.processResult.status));
          yaml_emit::scalarField(out, 8, "exit-code", member.processResult.exitCode);
          yaml_emit::scalarField(out, 8, "signal", member.processResult.signal);
          yaml_emit::boolField(out, 8, "usable", isUsableMemberResponse(member.processResult));
          yaml_emit::scalarField(out, 8, "review-stream", memberReviewStream(member.processResult));
          yaml_emit::boolField(out, 8, "stdout-truncated", member.processResult.standardOutputTruncated);
          yaml_emit::boolField(out, 8, "stderr-truncated", member.processResult.standardErrorTruncated);
          yaml_emit::scalarField(out, 8, "stdout", (memberPath / "stdout.txt").generic_string());
          yaml_emit::scalarField(out, 8, "stderr", (memberPath / "stderr.txt").generic_string());
          yaml_emit::scalarField(out, 8, "response", (memberPath / "response.md").generic_string());
        }
      }

      return out.str();
    }

    ReviewManifest dependencyManifest(PhaseIntent const& intent)
    {
      return ReviewManifest{.phaseId = intent.id,
                            .failure = FailureReason::DependencyFailed,
                            .summary = "Skipped because a dependency phase failed."};
    }

    Result<std::vector<ResolvedPhase>> resolvePhases(Registry const& registry, std::vector<PhaseIntent> const& intents)
    {
      auto result = std::vector<ResolvedPhase>{};
      result.reserve(intents.size());

      for (auto const& intent : intents)
      {
        auto resolved = resolvePhase(registry, intent);

        if (!resolved)
        {
          return std::unexpected{resolved.error()};
        }

        result.push_back(std::move(*resolved));
      }

      return result;
    }

    template<typename Function>
    async::Task<std::invoke_result_t<Function>> runOnWorker(async::Runtime* runtime, Function function)
    {
      co_await runtime->resumeOnWorker();
      co_return std::invoke(function);
    }

    template<typename Function>
    auto spawnWorker(async::Runtime& runtime, Function function)
    {
      return runtime.spawn(runOnWorker(&runtime, std::move(function)));
    }

    Error errorFromCurrentException()
    {
      try
      {
        throw;
      }
      catch (std::exception const& exception)
      {
        return Error{.code = Error::Code::IoError, .message = std::string{"worker raised: "} + exception.what()};
      }
      catch (...)
      {
        return Error{.code = Error::Code::IoError, .message = "worker raised an unknown exception"};
      }
    }

    template<typename T>
    Result<T> awaitOutcome(std::future<T>& future)
    {
      try
      {
        return future.get();
      }
      catch (...)
      {
        return std::unexpected{errorFromCurrentException()};
      }
    }

    template<typename T>
    std::vector<Result<T>> awaitAll(std::vector<std::future<T>>& futures)
    {
      auto outcomes = std::vector<Result<T>>{};
      outcomes.reserve(futures.size());

      for (auto& future : futures)
      {
        outcomes.push_back(awaitOutcome(future));
      }

      return outcomes;
    }

    struct MemberRun final
    {
      std::string agentId = {};
      std::string roundId = {};
      std::string roundLabel = {};
      ProcessResult result = {};
      std::optional<Error> optError = std::nullopt;
    };

    struct CouncilRoundResult final
    {
      std::vector<MemberRoundResult> memberResults = {};
      CouncilRows usableRows = {};
      std::optional<Error> optInfrastructureError = std::nullopt;
    };

    struct MemberLaunchSpec final
    {
      AgentDefinition agent = {};
      std::filesystem::path memberRelative = {};
      std::filesystem::path memberRoot = {};
      std::filesystem::path workspace = {};
      std::string prompt = {};
    };

    std::string_view draftRoundContext(Depth depth)
    {
      if (depth == Depth::Panel)
      {
        return "Produce an independent review. The chair synthesizes the council drafts.";
      }

      if (depth == Depth::Challenge)
      {
        return "Produce an independent review. Peer members will challenge the usable drafts.";
      }

      return "Produce an independent review. Peer members will challenge it, then usable members revise.";
    }

    std::string_view challengeRoundContext(Depth depth)
    {
      if (depth == Depth::Full)
      {
        return "Challenge the peer reviews below without assuming any is correct. "
               "These challenges go back to the draft authors for self-revision, so target concrete claims, "
               "missing evidence, and actionable corrections.";
      }

      return "Challenge the peer reviews below without assuming any is correct. "
             "These challenges go to the chair for synthesis, so call out broad risks, omissions, "
             "and disagreements that should shape the final review.";
    }

    CouncilContexts commonContexts(std::vector<std::string> const& roster, std::string_view context)
    {
      auto contexts = CouncilContexts{};

      for (auto const& member : roster)
      {
        contexts.emplace(member, context);
      }

      return contexts;
    }

    CouncilContexts peerContexts(std::vector<std::string> const& roster,
                                 std::string_view heading,
                                 CouncilRows const& rows)
    {
      auto contexts = CouncilContexts{};

      for (auto const& recipient : roster)
      {
        auto out = std::ostringstream{};
        std::println(out, "{}", heading);

        for (auto const& [member, text] : rows)
        {
          if (member != recipient)
          {
            std::print(out, "\n--- {} ---\n{}\n", member, text);
          }
        }

        contexts.emplace(recipient, std::move(out).str());
      }

      return contexts;
    }

    CouncilContexts revisionContexts(std::vector<std::string> const& roster,
                                     CouncilRows const& drafts,
                                     CouncilRows const& challenges)
    {
      auto contexts = peerContexts(roster,
                                   "Revise your original review after considering peer challenges below. "
                                   "Verify claims before accepting them.",
                                   challenges);

      for (auto const& recipient : roster)
      {
        auto const find = [&recipient](CouncilRows const& rows)
        { return std::ranges::find(rows, recipient, &std::pair<std::string, std::string>::first); };
        auto prefix = std::string{};

        if (auto draft = find(drafts); draft != drafts.end())
        {
          prefix += std::format("Your original review:\n{}\n\n", draft->second);
        }

        if (auto challenge = find(challenges); challenge != challenges.end())
        {
          prefix += std::format("Your own challenge notes:\n{}\n\n", challenge->second);
        }

        contexts[recipient] = prefix + contexts.at(recipient);
      }

      return contexts;
    }

    std::vector<std::string> usableRoster(CouncilRows const& rows)
    {
      auto result = std::vector<std::string>{};
      result.reserve(rows.size());

      for (auto const& [member, text] : rows)
      {
        result.push_back(member);
      }

      return result;
    }

    std::size_t roundCount(Depth depth)
    {
      switch (depth)
      {
        case Depth::Panel: return 1;
        case Depth::Challenge: return 2;
        case Depth::Full: return 3;
      }

      return 1;
    }

    std::string roundLabel(std::size_t round, std::size_t total, std::string_view name)
    {
      return std::format("{} of {} ({})", round, total, name);
    }

    MemberRun runMemberSandbox(EngineContext const& context,
                               ArtifactStore const& store,
                               MemberLaunchSpec launch,
                               std::string const& roundId,
                               std::string const& roundLabel,
                               std::string const& phaseId)
    {
      auto run = MemberRun{.agentId = launch.agent.id, .roundId = roundId, .roundLabel = roundLabel};
      auto workerSnapshot = SnapshotProvider{context.processRunner};
      auto workspaceResult = workerSnapshot.createWorkspace(context.immutableBase, launch.workspace);

      if (!workspaceResult)
      {
        run.result =
          ProcessResult{.status = ProcessStatus::LaunchFailed, .standardError = workspaceResult.error().message};
        run.optError = workspaceResult.error();
      }
      else
      {
        auto workspacePromptStore = ArtifactStore{*workspaceResult};

        if (auto workspacePrompt = writeArtifact(workspacePromptStore, kWorkspacePromptFile, launch.prompt);
            !workspacePrompt)
        {
          run.result =
            ProcessResult{.status = ProcessStatus::LaunchFailed, .standardError = workspacePrompt.error().message};
          run.optError = workspacePrompt.error();
        }
        else
        {
          auto const sandboxPromptPath = context.realRepo / kWorkspacePromptFile;
          auto request = requestFor(launch.agent,
                                    *workspaceResult,
                                    sandboxPromptPath,
                                    launch.prompt,
                                    launch.memberRoot / "stdout.txt",
                                    launch.memberRoot / "stderr.txt");
          request.onLaunch = [&store, phaseId, agentId = launch.agent.id, roundId](std::int64_t pid)
          {
            [[maybe_unused]] auto const ignored = appendTrace(
              store,
              "member-launched",
              {{"phase-id", phaseId}, {"agent", agentId}, {"round", roundId}, {"pid", std::format("{}", pid)}});
          };

          auto runner = NamespaceRunner{context.processRunner};
          run.result = runner.run(context.realRepo, *workspaceResult, SandboxMounts{}, std::move(request));

          if (run.result.status == ProcessStatus::LaunchFailed && !run.optError)
          {
            run.optError = Error{.code = Error::Code::IoError,
                                 .message = std::format(
                                   "sandbox launch failed for agent '{}': {}", run.agentId, run.result.standardError)};
          }
        }
      }

      if (auto write = writeArtifact(store, launch.memberRelative / "response.md", memberResponse(run.result));
          !write && !run.optError)
      {
        run.optError = write.error();
      }

      if (auto trace = appendTrace(store,
                                   "member-finished",
                                   {{"phase-id", phaseId},
                                    {"agent", run.agentId},
                                    {"round", run.roundId},
                                    {"status", std::string{toString(run.result.status)}},
                                    {"exit-code", std::format("{}", run.result.exitCode)}});
          !trace && !run.optError)
      {
        run.optError = trace.error();
      }

      return run;
    }

    Result<CouncilRoundResult> runCouncilRound(ResolvedPhase const& phase,
                                               EngineContext const& context,
                                               ArtifactStore const& store,
                                               std::filesystem::path const& phaseRoot,
                                               std::vector<std::string> const& roster,
                                               std::string const& roundId,
                                               std::string const& roundLabel,
                                               CouncilContexts const& memberContexts)
    {
      auto memberFutures = std::vector<std::future<MemberRun>>{};
      memberFutures.reserve(roster.size());
      auto launches = std::vector<MemberLaunchSpec>{};
      launches.reserve(roster.size());

      for (auto const& agentId : roster)
      {
        auto agentIt = context.registry.agents.find(agentId);

        if (agentIt == context.registry.agents.end())
        {
          return makeError(Error::Code::InvalidState, std::format("unknown council agent '{}'", agentId));
        }

        auto agent = agentIt->second;
        auto const memberRelative = std::filesystem::path{"members"} / agent.id / roundId;
        auto const memberRoot = phaseRoot / memberRelative;
        auto const workspace = memberRoot / "workspace";

        auto const contextIt = memberContexts.find(agent.id);
        auto const roundContext = contextIt == memberContexts.end() ? std::string{} : contextIt->second;
        auto const prompt = promptFor(phase, agent, roundLabel, roundContext);

        if (auto result = writeArtifact(store, memberRelative / "prompt.md", prompt); !result)
        {
          return std::unexpected{result.error()};
        }

        if (auto result = appendTrace(
              store, "member-started", {{"phase-id", phase.intent.id}, {"agent", agent.id}, {"round", roundId}});
            !result)
        {
          return std::unexpected{result.error()};
        }

        launches.push_back(MemberLaunchSpec{.agent = std::move(agent),
                                            .memberRelative = memberRelative,
                                            .memberRoot = memberRoot,
                                            .workspace = workspace,
                                            .prompt = prompt});
      }

      for (auto& launch : launches)
      {
        memberFutures.push_back(spawnWorker(
          context.asyncRuntime,
          [&context, &store, launch = std::move(launch), roundId, roundLabel, phaseId = phase.intent.id] mutable
          { return runMemberSandbox(context, store, std::move(launch), roundId, roundLabel, phaseId); }));
      }

      auto roundResult = CouncilRoundResult{};

      for (auto& futureOutcome : awaitAll(memberFutures))
      {
        if (!futureOutcome)
        {
          if (!roundResult.optInfrastructureError)
          {
            roundResult.optInfrastructureError = futureOutcome.error();
          }

          continue;
        }

        auto run = std::move(*futureOutcome);

        if (run.optError && !roundResult.optInfrastructureError)
        {
          roundResult.optInfrastructureError = *run.optError;
        }

        if (isUsableMemberResponse(run.result))
        {
          roundResult.usableRows.emplace_back(run.agentId, std::string{memberReviewText(run.result)});
        }

        roundResult.memberResults.push_back(MemberRoundResult{.agentId = std::move(run.agentId),
                                                              .roundId = std::move(run.roundId),
                                                              .roundLabel = std::move(run.roundLabel),
                                                              .processResult = std::move(run.result)});
      }

      return roundResult;
    }

    Result<> writePhaseArtifacts(ArtifactStore const& store,
                                 ResolvedPhase const& phase,
                                 std::vector<MemberRoundResult> const& memberResults,
                                 ReviewManifest const& manifest)
    {
      if (auto result = writeArtifact(store, "evidence.yaml", evidenceFor(phase, memberResults)); !result)
      {
        return std::unexpected{result.error()};
      }

      if (auto result = writeArtifact(store, "dossier.md", dossierFor(phase, memberResults)); !result)
      {
        return std::unexpected{result.error()};
      }

      if (auto result = writeArtifact(store, "manifest.yaml", emitManifest(manifest)); !result)
      {
        return std::unexpected{result.error()};
      }

      return appendTrace(store,
                         "phase-completed",
                         {{"phase-id", phase.intent.id}, {"failure", std::string{toString(manifest.failure)}}});
    }

    Result<> writeInitialPhaseArtifacts(ArtifactStore const& store, ResolvedPhase const& phase)
    {
      if (auto result = writeArtifact(store, "intent.yaml", emitIntent(phase.intent)); !result)
      {
        return std::unexpected{result.error()};
      }

      if (auto result = writeArtifact(store, "resolved.yaml", emitResolved(phase)); !result)
      {
        return std::unexpected{result.error()};
      }

      return appendTrace(store, "phase-started", {{"phase-id", phase.intent.id}});
    }

    Result<> writeSkippedPhaseArtifacts(std::filesystem::path const& runRoot,
                                        ResolvedPhase const& phase,
                                        ReviewManifest const& manifest)
    {
      auto store = ArtifactStore{runRoot / phase.intent.id};

      if (auto result = writeInitialPhaseArtifacts(store, phase); !result)
      {
        return std::unexpected{result.error()};
      }

      return writePhaseArtifacts(store, phase, {}, manifest);
    }

    Result<> rewriteManifestsForRealTreeChange(std::filesystem::path const& runRoot, RunSummary& summary)
    {
      for (auto& manifest : summary.manifests)
      {
        manifest.failure = FailureReason::RealTreeChanged;
        manifest.summary = "The real repository changed during the council run; delegated results were discarded.";
        auto store = ArtifactStore{runRoot / manifest.phaseId};

        if (auto written = writeArtifact(store, "manifest.yaml", emitManifest(manifest)); !written)
        {
          return std::unexpected{written.error()};
        }

        if (auto traced =
              appendTrace(store,
                          "run-discarded",
                          {{"phase-id", manifest.phaseId}, {"failure", std::string{toString(manifest.failure)}}});
            !traced)
        {
          return std::unexpected{traced.error()};
        }
      }

      summary.failed = true;
      return {};
    }

    Result<std::optional<ReviewManifest>> tryRunIntent(Engine& engine,
                                                       EngineContext const& context,
                                                       ResolvedPhase const& phase,
                                                       std::map<std::string, bool, std::less<>>& completed,
                                                       std::set<std::string>& launched,
                                                       std::filesystem::path const& out)
    {
      auto const& intent = phase.intent;

      if (completed.contains(intent.id) || launched.contains(intent.id))
      {
        return std::nullopt;
      }

      bool ready = true;
      bool dependencyFailed = false;

      for (auto const& dependency : intent.dependsOn)
      {
        auto dependencyIt = completed.find(dependency);

        if (dependencyIt == completed.end())
        {
          ready = false;
          break;
        }

        dependencyFailed = dependencyFailed || !dependencyIt->second;
      }

      if (!ready)
      {
        return std::nullopt;
      }

      launched.insert(intent.id);

      auto manifest = ReviewManifest{};

      if (dependencyFailed)
      {
        manifest = dependencyManifest(intent);

        if (auto result = writeSkippedPhaseArtifacts(out, phase, manifest); !result)
        {
          return std::unexpected{result.error()};
        }
      }
      else
      {
        auto executed = engine.execute(phase, context);

        if (!executed)
        {
          return std::unexpected{executed.error()};
        }

        manifest = std::move(*executed);
      }

      return manifest;
    }
  } // namespace

  Result<> Scheduler::validate(std::vector<PhaseIntent> const& intents)
  {
    auto ids = std::set<std::string>{};

    for (auto const& intent : intents)
    {
      if (!ids.insert(intent.id).second)
      {
        return makeError(Error::Code::InvalidState, std::format("duplicate phase id '{}'", intent.id));
      }
    }

    for (auto const& intent : intents)
    {
      for (auto const& dependency : intent.dependsOn)
      {
        if (!ids.contains(dependency))
        {
          return makeError(
            Error::Code::InvalidState, std::format("phase '{}' depends on unknown phase '{}'", intent.id, dependency));
        }
      }
    }

    auto visiting = std::set<std::string>{};
    auto visited = std::set<std::string>{};
    auto byId = std::map<std::string, PhaseIntent const*, std::less<>>{};

    for (auto const& intent : intents)
    {
      byId.emplace(intent.id, &intent);
    }

    auto visit = [&](auto const& self, std::string const& id) -> Result<>
    {
      if (visited.contains(id))
      {
        return {};
      }

      if (!visiting.insert(id).second)
      {
        return makeError(Error::Code::InvalidState, std::format("dependency cycle includes '{}'", id));
      }

      for (auto const& dependency : byId.at(id)->dependsOn)
      {
        if (auto result = self(self, dependency); !result)
        {
          return std::unexpected{result.error()};
        }
      }

      visiting.erase(id);
      visited.insert(id);
      return {};
    };

    for (auto const& intent : intents)
    {
      if (auto result = visit(visit, intent.id); !result)
      {
        return result;
      }
    }

    return {};
  }

  Result<ReviewManifest> Engine::execute(ResolvedPhase const& phase, EngineContext const& context)
  {
    auto phaseRoot = context.runRoot / phase.intent.id;
    auto store = ArtifactStore{phaseRoot};

    if (auto result = writeInitialPhaseArtifacts(store, phase); !result)
    {
      return std::unexpected{result.error()};
    }

    auto memberResults = std::vector<MemberRoundResult>{};
    memberResults.reserve(phase.definition.parameters.roster.size() * 3);
    auto const totalRounds = roundCount(phase.definition.parameters.depth);
    auto recordInfrastructureFailure = [&](Error const& error) -> Result<ReviewManifest>
    {
      auto manifest = ReviewManifest{
        .phaseId = phase.intent.id, .failure = FailureReason::InfrastructureFailed, .summary = error.message};

      if (auto written = writePhaseArtifacts(store, phase, memberResults, manifest); !written)
      {
        return std::unexpected{written.error()};
      }

      return manifest;
    };

    auto draftRound = runCouncilRound(
      phase,
      context,
      store,
      phaseRoot,
      phase.definition.parameters.roster,
      "r1",
      roundLabel(1, totalRounds, "independent review"),
      commonContexts(phase.definition.parameters.roster, draftRoundContext(phase.definition.parameters.depth)));

    if (!draftRound)
    {
      return std::unexpected{draftRound.error()};
    }

    memberResults.insert(memberResults.end(),
                         std::make_move_iterator(draftRound->memberResults.begin()),
                         std::make_move_iterator(draftRound->memberResults.end()));

    if (draftRound->optInfrastructureError && draftRound->usableRows.size() < phase.definition.parameters.quorum)
    {
      return recordInfrastructureFailure(*draftRound->optInfrastructureError);
    }

    auto finalRows = draftRound->usableRows;

    if (draftRound->usableRows.size() >= phase.definition.parameters.quorum && totalRounds >= 2)
    {
      auto roster = usableRoster(draftRound->usableRows);
      auto challengeRound = runCouncilRound(
        phase,
        context,
        store,
        phaseRoot,
        roster,
        "r2",
        roundLabel(2, totalRounds, "cross-challenge"),
        peerContexts(roster, challengeRoundContext(phase.definition.parameters.depth), draftRound->usableRows));

      if (!challengeRound)
      {
        return std::unexpected{challengeRound.error()};
      }

      memberResults.insert(memberResults.end(),
                           std::make_move_iterator(challengeRound->memberResults.begin()),
                           std::make_move_iterator(challengeRound->memberResults.end()));

      if (challengeRound->optInfrastructureError &&
          challengeRound->usableRows.size() < phase.definition.parameters.quorum)
      {
        return recordInfrastructureFailure(*challengeRound->optInfrastructureError);
      }

      finalRows = challengeRound->usableRows;

      if (challengeRound->usableRows.size() >= phase.definition.parameters.quorum && totalRounds >= 3)
      {
        roster = usableRoster(challengeRound->usableRows);
        auto revisionRound =
          runCouncilRound(phase,
                          context,
                          store,
                          phaseRoot,
                          roster,
                          "r3",
                          roundLabel(3, totalRounds, "self-revision"),
                          revisionContexts(roster, draftRound->usableRows, challengeRound->usableRows));

        if (!revisionRound)
        {
          return std::unexpected{revisionRound.error()};
        }

        memberResults.insert(memberResults.end(),
                             std::make_move_iterator(revisionRound->memberResults.begin()),
                             std::make_move_iterator(revisionRound->memberResults.end()));

        if (revisionRound->optInfrastructureError &&
            revisionRound->usableRows.size() < phase.definition.parameters.quorum)
        {
          return recordInfrastructureFailure(*revisionRound->optInfrastructureError);
        }

        finalRows = revisionRound->usableRows;
      }
    }

    auto const successCount = finalRows.size();

    auto manifest = ReviewManifest{
      .phaseId = phase.intent.id,
      .failure = successCount >= phase.definition.parameters.quorum ? FailureReason::None : FailureReason::QuorumFailed,
      .summary = std::format(
        "{} of {} council members returned usable responses", successCount, phase.definition.parameters.roster.size())};

    if (auto result = writePhaseArtifacts(store, phase, memberResults, manifest); !result)
    {
      return std::unexpected{result.error()};
    }

    return manifest;
  }

  Runner::Runner(ProcessRunner& processRunner)
    : _processRunner{processRunner}
  {
  }

  Result<RunSummary> Runner::run(Registry const& registry,
                                 std::vector<PhaseIntent> const& intents,
                                 std::filesystem::path const& repo,
                                 std::filesystem::path const& out)
  {
    if (auto result = Scheduler::validate(intents); !result)
    {
      return std::unexpected{result.error()};
    }

    auto phases = resolvePhases(registry, intents);

    if (!phases)
    {
      return std::unexpected{phases.error()};
    }

    auto paths = prepareRunPaths(repo, out);

    if (!paths)
    {
      return std::unexpected{paths.error()};
    }

    auto const before = TreeCanary::fingerprint(paths->repo);

    if (!before)
    {
      return std::unexpected{before.error()};
    }

    auto snapshot = SnapshotProvider{_processRunner};
    auto immutableBase = snapshot.createImmutableBase(paths->repo, paths->out / ".base");

    if (!immutableBase)
    {
      return std::unexpected{immutableBase.error()};
    }

    // The immutable base is a full committed copy of the repo used only as the rsync source for per-member
    // workspaces. Nothing inspects it after the run, so reclaim it on every exit path. Declared before the
    // async runtime below so its worker threads are joined (and done copying from the base) before removal.
    auto const reclaimBase = ScopeExit{[&] { snapshot.remove(*immutableBase); }};

    auto summary = RunSummary{};
    auto completed = std::map<std::string, bool, std::less<>>{};
    auto launched = std::set<std::string>{};
    auto engine = Engine{};
    auto callbackExecutor = async::ImmediateExecutor{};
    auto asyncRuntime = async::Runtime{callbackExecutor, 16};
    auto context = EngineContext{.realRepo = paths->repo,
                                 .immutableBase = *immutableBase,
                                 .runRoot = paths->out,
                                 .registry = registry,
                                 .processRunner = _processRunner,
                                 .asyncRuntime = asyncRuntime};

    while (completed.size() < phases->size())
    {
      bool progressed = false;

      for (auto const& phase : *phases)
      {
        auto runResult = tryRunIntent(engine, context, phase, completed, launched, paths->out);

        if (!runResult)
        {
          return std::unexpected{runResult.error()};
        }

        if (auto& optManifest = *runResult; optManifest)
        {
          progressed = true;
          auto manifest = std::move(*optManifest);
          auto const succeeded = manifest.failure == FailureReason::None;
          completed[phase.intent.id] = succeeded;
          summary.failed = summary.failed || !succeeded;
          summary.manifests.push_back(std::move(manifest));
        }
      }

      if (!progressed)
      {
        return makeError(Error::Code::InvalidState, "scheduler made no progress");
      }
    }

    auto const after = TreeCanary::fingerprint(paths->repo);

    if (!after)
    {
      return std::unexpected{after.error()};
    }

    if (*before != *after)
    {
      if (auto rewritten = rewriteManifestsForRealTreeChange(paths->out, summary); !rewritten)
      {
        return std::unexpected{rewritten.error()};
      }

      return makeError(Error::Code::Conflict, "real repository changed during council run");
    }

    return summary;
  }
} // namespace ao::council
