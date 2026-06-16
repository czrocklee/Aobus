// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "fleet/Serialization.h"

#include "fleet/Model.h"
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/yaml/Utils.h>

#include <c4/yml/common.hpp>
#include <ryml.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <ios>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace ao::fleet
{
  namespace
  {
    constexpr auto kMaxDocumentBytes = std::size_t{2UL * 1024 * 1024};
    constexpr auto kMaxDepth = std::size_t{32};
    constexpr auto kMaxCollectionEntries = std::size_t{20'000};
    constexpr auto kAsciiControlBound = 0x20;
    constexpr auto kAsciiDel = 0x7F;

    class ParseFailure final : public Exception
    {
    public:
      using Exception::Exception;
    };

    std::unexpected<Error> validationError(Error::Code code, std::string context, std::string message)
    {
      if (!context.empty())
      {
        message = std::move(context) + ": " + std::move(message);
      }

      return makeError(code, std::move(message));
    }

    std::unexpected<Error> validationError(std::string context, std::string message)
    {
      return validationError(Error::Code::InvalidState, std::move(context), std::move(message));
    }

    Result<std::string> readDocument(std::filesystem::path const& path)
    {
      auto input = std::ifstream{path, std::ios::binary};

      if (!input)
      {
        return validationError(Error::Code::IoError, path.string(), "cannot open file");
      }

      input.seekg(0, std::ios::end);
      auto const length = input.tellg();

      if (length < 0 || std::cmp_greater(static_cast<std::uint64_t>(length), kMaxDocumentBytes))
      {
        return validationError(path.string(), "document exceeds 2 MiB limit");
      }

      input.seekg(0);
      auto value = std::string(static_cast<std::size_t>(length), '\0');
      input.read(value.data(), static_cast<std::streamsize>(length));

      if (!input && length != 0)
      {
        return validationError(Error::Code::IoError, path.string(), "cannot read file");
      }

      return value;
    }

    // Rejects YAML language extensions on the parsed tree instead of scanning raw text,
    // so '&', '*', and '!' inside scalar content (code snippets, glob patterns) survive
    // while real anchors, aliases, tags, and merge keys are still refused.
    void rejectYamlExtensions(ryml::ConstNodeRef node)
    {
      if (node.has_key_anchor() || node.has_val_anchor())
      {
        throwException<ParseFailure>("YAML anchors are forbidden");
      }

      if (node.is_key_ref() || node.is_val_ref())
      {
        throwException<ParseFailure>("YAML aliases are forbidden");
      }

      if (node.has_key_tag() || node.has_val_tag())
      {
        throwException<ParseFailure>("YAML tags are forbidden");
      }

      if (node.has_key() && node.key() == "<<")
      {
        throwException<ParseFailure>("YAML merge keys are forbidden");
      }
    }

    void validateTree(ryml::ConstNodeRef node, std::size_t depth, std::size_t& entries)
    {
      if (depth > kMaxDepth)
      {
        throwException<ParseFailure>("YAML nesting exceeds limit");
      }

      rejectYamlExtensions(node);

      if (node.is_map())
      {
        auto keys = std::set<std::string>{};

        for (auto child : node.children())
        {
          auto const key = child.key();

          if (auto keyString = std::string{key.data(), key.size()}; !keys.insert(keyString).second)
          {
            throwException<ParseFailure>("duplicate key '{}'", keyString);
          }
        }
      }

      for (auto child : node.children())
      {
        ++entries;

        if (entries > kMaxCollectionEntries)
        {
          throwException<ParseFailure>("YAML collection entry limit exceeded");
        }

        validateTree(child, depth + 1, entries);
      }
    }

    struct ParsedYaml final
    {
      std::string source;
      ryml::Tree tree;

      ParsedYaml()
        : tree{yaml::callbacks()}
      {
      }
    };

    Result<ParsedYaml> parseYaml(std::filesystem::path const& path)
    {
      auto source = readDocument(path);

      if (!source)
      {
        return std::unexpected{source.error()};
      }

      try
      {
        auto parsed = ParsedYaml{};
        parsed.source = std::move(*source);
        ryml::parse_in_arena(yaml::toCsubstr(parsed.source), &parsed.tree);
        auto entries = std::size_t{};
        validateTree(parsed.tree.rootref(), 0, entries);
        return parsed;
      }
      catch (std::exception const& exception)
      {
        return validationError(path.string(), exception.what());
      }
    }

    ryml::ConstNodeRef documentRoot(ryml::Tree const& tree)
    {
      auto root = tree.rootref();

      if (root.is_stream())
      {
        if (root.num_children() != 1)
        {
          throwException<ParseFailure>("expected exactly one YAML document");
        }

        return root.first_child();
      }

      return root;
    }

    std::string scalar(ryml::ConstNodeRef node, std::string_view context)
    {
      if (!node.readable() || !node.has_val() || node.is_container())
      {
        throwException<ParseFailure>("{} must be a scalar", context);
      }

      return std::string{yaml::scalarView(node)};
    }

    std::string key(ryml::ConstNodeRef node)
    {
      return std::string{yaml::keyView(node)};
    }

    ryml::ConstNodeRef required(ryml::ConstNodeRef node, std::string_view name)
    {
      auto child = yaml::findChild(node, name);

      if (!child.readable())
      {
        throwException<ParseFailure>("missing required field '{}'", name);
      }

      return child;
    }

    ryml::ConstNodeRef optional(ryml::ConstNodeRef node, std::string_view name)
    {
      return yaml::findChild(node, name);
    }

    void requireMap(ryml::ConstNodeRef node, std::string_view context)
    {
      if (!node.readable() || !node.is_map())
      {
        throwException<ParseFailure>("{} must be a map", context);
      }
    }

    void requireSequence(ryml::ConstNodeRef node, std::string_view context)
    {
      if (!node.readable() || !node.is_seq())
      {
        throwException<ParseFailure>("{} must be a sequence", context);
      }
    }

    void rejectUnknown(ryml::ConstNodeRef node, std::vector<std::string_view> const& allowed, std::string_view context)
    {
      requireMap(node, context);

      for (auto child : node.children())
      {
        if (auto const childKey = key(child); !std::ranges::contains(allowed, childKey))
        {
          throwException<ParseFailure>("unknown field '{}' in {}", childKey, context);
        }
      }
    }

    void rejectUnknown(ryml::ConstNodeRef node,
                       std::initializer_list<std::string_view> allowed,
                       std::string_view context)
    {
      rejectUnknown(node, std::vector<std::string_view>{allowed}, context);
    }

    std::size_t unsignedValue(ryml::ConstNodeRef node, std::string_view context)
    {
      auto const text = scalar(node, context);
      auto result = std::size_t{};
      auto const conversion = std::from_chars(text.data(), text.data() + text.size(), result);

      if (conversion.ec != std::errc{} || conversion.ptr != text.data() + text.size())
      {
        throwException<ParseFailure>("{} must be an unsigned integer", context);
      }

      return result;
    }

    template<typename Enum, std::size_t N>
    Enum closedEnum(ryml::ConstNodeRef node, std::string_view context, EnumNameTable<Enum, N> const& names)
    {
      auto const text = scalar(node, context);

      if (auto const optValue = enumFromName(names, text); optValue)
      {
        return *optValue;
      }

      throwException<ParseFailure>("invalid {} '{}'", context, text);
    }

    ScopeOperation parseOperation(ryml::ConstNodeRef node)
    {
      return closedEnum(node, "scope operation", kScopeOperationNames);
    }

    EngineKind parseEngine(ryml::ConstNodeRef node)
    {
      return closedEnum(node, "engine", kEngineKindNames);
    }

    PromptDelivery parsePromptDelivery(ryml::ConstNodeRef node)
    {
      return closedEnum(node, "prompt delivery", kPromptDeliveryNames);
    }

    OracleRunner parseOracleRunner(ryml::ConstNodeRef node)
    {
      return closedEnum(node, "oracle runner", kOracleRunnerNames);
    }

    BaselinePolicy parseBaseline(ryml::ConstNodeRef node)
    {
      return closedEnum(node, "baseline policy", kBaselinePolicyNames);
    }

    FailureReason parseFailureReason(ryml::ConstNodeRef node)
    {
      auto const result = closedEnum(node, "failure reason", kFailureReasonNames);

      // 'none' marks a successful phase; an escalation rule keyed on it would be meaningless.
      if (result == FailureReason::None)
      {
        throwException<ParseFailure>("invalid failure reason 'none'");
      }

      return result;
    }

    EscalationAction parseEscalationAction(ryml::ConstNodeRef node)
    {
      return closedEnum(node, "escalation action", kEscalationActionNames);
    }

    CouncilDepth parseCouncilDepth(ryml::ConstNodeRef node)
    {
      return closedEnum(node, "synthesis depth", kCouncilDepthNames);
    }

    std::vector<std::string> stringSequence(ryml::ConstNodeRef node, std::string_view context)
    {
      requireSequence(node, context);
      auto result = std::vector<std::string>{};
      result.reserve(node.num_children());

      for (auto child : node.children())
      {
        result.push_back(scalar(child, context));
      }

      return result;
    }

    std::vector<std::filesystem::path> pathSequence(ryml::ConstNodeRef node, std::string_view context)
    {
      auto strings = stringSequence(node, context);
      auto result = std::vector<std::filesystem::path>{};
      result.reserve(strings.size());

      for (auto& value : strings)
      {
        result.emplace_back(std::move(value));
      }

      return result;
    }

    std::filesystem::path safeRelativePath(std::string value, std::string_view context)
    {
      auto path = std::filesystem::path{std::move(value)};

      if (path.empty() || path.is_absolute())
      {
        throwException<ParseFailure>("{} must be a non-empty relative path", context);
      }

      auto normalized = path.lexically_normal();

      if (normalized.empty() || *normalized.begin() == "..")
      {
        throwException<ParseFailure>("{} contains path traversal", context);
      }

      return normalized;
    }

    void requireSafeIdentifier(std::string_view value, std::string_view context)
    {
      constexpr std::size_t kMaxIdentifierLength = 128;

      if (value.empty() || value.size() > kMaxIdentifierLength || value == "." || value == "..")
      {
        throwException<ParseFailure>("{} must be a non-empty safe identifier", context);
      }

      auto const safe = std::ranges::all_of(value,
                                            [](char character)
                                            {
                                              auto const byte = static_cast<unsigned char>(character);
                                              return std::isalnum(byte) != 0 || character == '-' || character == '_' ||
                                                     character == '.';
                                            });

      if (!safe)
      {
        throwException<ParseFailure>("{} contains unsafe characters", context);
      }
    }

    IntentOverrides parseOverrides(ryml::ConstNodeRef node)
    {
      auto allowed = std::vector<std::string_view>{};
      forEachOverrideField([&](std::string_view name, auto /*member*/, OverridePolicy /*policy*/, auto /*target*/)
                           { allowed.push_back(name); });
      rejectUnknown(node, allowed, "intent overrides");
      auto result = IntentOverrides{};
      forEachOverrideField(
        [&](std::string_view name, auto member, OverridePolicy /*policy*/, auto /*target*/)
        {
          auto child = optional(node, name);

          if (!child.readable())
          {
            return;
          }

          using Value = typename std::remove_cvref_t<decltype(result.*member)>::value_type;

          if constexpr (std::same_as<Value, std::string>)
          {
            result.*member = scalar(child, name);
          }
          else if constexpr (std::same_as<Value, std::size_t>)
          {
            result.*member = unsignedValue(child, name);
          }
          else if constexpr (std::same_as<Value, EngineKind>)
          {
            result.*member = closedEnum(child, name, kEngineKindNames);
          }
          else
          {
            static_assert(std::same_as<Value, CouncilDepth>);
            result.*member = closedEnum(child, name, kCouncilDepthNames);
          }
        });
      return result;
    }

    PhaseIntent parseIntentRoot(ryml::ConstNodeRef root)
    {
      rejectUnknown(
        root, {"schema", "id", "task-kind", "invariant", "scope", "depends-on", "overrides", "body"}, "intent");

      if (scalar(required(root, "schema"), "schema") != "aobus-fleet-intent/v1")
      {
        throwException<ParseFailure>("unsupported intent schema");
      }

      auto result = PhaseIntent{};

      if (auto id = optional(root, "id"); id.readable())
      {
        result.id = scalar(id, "id");
      }

      if (result.id.empty())
      {
        result.id = makePhaseId();
      }

      requireSafeIdentifier(result.id, "intent id");
      result.taskKind = scalar(required(root, "task-kind"), "task-kind");
      requireSafeIdentifier(result.taskKind, "task-kind");
      result.invariant = scalar(required(root, "invariant"), "invariant");

      if (result.invariant.empty())
      {
        throwException<ParseFailure>("invariant must not be empty");
      }

      auto scope = required(root, "scope");
      requireSequence(scope, "scope");
      auto scopePaths = std::set<std::filesystem::path>{};

      for (auto row : scope.children())
      {
        rejectUnknown(row, {"path", "operations"}, "scope row");
        auto rule = ScopeRule{};
        rule.path = safeRelativePath(scalar(required(row, "path"), "scope path"), "scope path");

        if (!scopePaths.insert(rule.path).second)
        {
          throwException<ParseFailure>("duplicate scope path '{}'", rule.path.generic_string());
        }

        auto operations = required(row, "operations");
        requireSequence(operations, "scope operations");

        for (auto operation : operations.children())
        {
          rule.operations.insert(parseOperation(operation));
        }

        if (rule.operations.empty())
        {
          throwException<ParseFailure>("scope operations must not be empty");
        }

        result.scope.push_back(std::move(rule));
      }

      result.dependsOn = stringSequence(required(root, "depends-on"), "depends-on");

      for (auto const& dependency : result.dependsOn)
      {
        requireSafeIdentifier(dependency, "dependency id");
      }

      if (auto overrides = optional(root, "overrides"); overrides.readable())
      {
        result.overrides = parseOverrides(overrides);
      }

      result.body = scalar(required(root, "body"), "body");

      if (result.body.empty())
      {
        throwException<ParseFailure>("body must not be empty");
      }

      return result;
    }

    HarnessDefinition parseHarness(std::string id, ryml::ConstNodeRef node)
    {
      rejectUnknown(
        node, {"argv", "prompt-delivery", "environment-whitelist", "timeout-ms", "rate-limit-key"}, "harness");
      auto result = HarnessDefinition{};
      result.id = std::move(id);
      result.argvTemplate = stringSequence(required(node, "argv"), "harness argv");

      if (result.argvTemplate.empty())
      {
        throwException<ParseFailure>("harness argv must not be empty");
      }

      result.promptDelivery = parsePromptDelivery(required(node, "prompt-delivery"));
      result.environmentWhitelist = stringSequence(required(node, "environment-whitelist"), "environment whitelist");
      result.timeout = std::chrono::milliseconds{unsignedValue(required(node, "timeout-ms"), "timeout-ms")};
      result.rateLimitKey = scalar(required(node, "rate-limit-key"), "rate-limit-key");

      if (result.rateLimitKey.empty())
      {
        throwException<ParseFailure>("harness '{}' rate-limit-key must not be empty", result.id);
      }

      return result;
    }

    std::string substitutePlaceholder(std::string value, std::string_view placeholder, std::string_view replacement)
    {
      auto cursor = std::size_t{};

      while ((cursor = value.find(placeholder, cursor)) != std::string::npos)
      {
        value.replace(cursor, placeholder.size(), replacement);
        cursor += replacement.size();
      }

      return value;
    }

    bool argvUsesPlaceholder(std::vector<std::string> const& argvTemplate, std::string_view placeholder)
    {
      return std::ranges::any_of(
        argvTemplate, [&](std::string const& argument) { return argument.find(placeholder) != std::string::npos; });
    }

    AgentDefinition parseAgent(std::string id,
                               ryml::ConstNodeRef node,
                               std::map<std::string, HarnessDefinition, std::less<>> const& harnesses)
    {
      rejectUnknown(node, {"harness", "model", "vendor", "effort", "timeout-ms"}, "agent");
      auto result = AgentDefinition{};
      result.id = std::move(id);
      result.harness = scalar(required(node, "harness"), "agent harness");
      result.model = scalar(required(node, "model"), "agent model");
      result.vendor = scalar(required(node, "vendor"), "agent vendor");

      if (result.model.empty())
      {
        throwException<ParseFailure>("agent '{}' model must not be empty", result.id);
      }

      if (result.vendor.empty())
      {
        throwException<ParseFailure>("agent '{}' vendor must not be empty", result.id);
      }

      if (auto effort = optional(node, "effort"); effort.readable())
      {
        result.effort = scalar(effort, "agent effort");

        if (result.effort.empty())
        {
          throwException<ParseFailure>("agent '{}' effort must not be empty", result.id);
        }
      }

      // Model and effort values reach the spawned command line via placeholder substitution;
      // braces inside them could smuggle tokens past the harness placeholder allow-list.
      for (auto const* field : {&result.model, &result.effort})
      {
        if (field->find('{') != std::string::npos || field->find('}') != std::string::npos)
        {
          throwException<ParseFailure>("agent '{}' model and effort must not contain placeholder braces", result.id);
        }
      }

      auto harnessIt = harnesses.find(result.harness);

      if (harnessIt == harnesses.end())
      {
        throwException<ParseFailure>("agent '{}' references unknown harness '{}'", result.id, result.harness);
      }

      auto const& harness = harnessIt->second;
      auto const usesEffort = argvUsesPlaceholder(harness.argvTemplate, "{effort}");

      if (usesEffort && result.effort.empty())
      {
        throwException<ParseFailure>(
          "agent '{}' must set effort: harness '{}' argv uses '{{effort}}'", result.id, harness.id);
      }

      if (!usesEffort && !result.effort.empty())
      {
        throwException<ParseFailure>(
          "agent '{}' sets effort but harness '{}' argv does not use '{{effort}}'", result.id, harness.id);
      }

      // The model and effort are static per agent, so they resolve at load time; only the
      // run-time placeholders (prompt, workspace, ...) survive into the engine.
      for (auto const& argument : harness.argvTemplate)
      {
        auto resolved = substitutePlaceholder(argument, "{model}", result.model);
        result.argvTemplate.push_back(substitutePlaceholder(std::move(resolved), "{effort}", result.effort));
      }

      result.promptDelivery = harness.promptDelivery;
      result.environmentWhitelist = harness.environmentWhitelist;
      result.timeout = harness.timeout;
      result.rateLimitKey = harness.rateLimitKey;

      if (auto timeout = optional(node, "timeout-ms"); timeout.readable())
      {
        result.timeout = std::chrono::milliseconds{unsignedValue(timeout, "agent timeout-ms")};
      }

      return result;
    }

    OracleDefinition parseOracle(std::string id, ryml::ConstNodeRef node)
    {
      rejectUnknown(node,
                    {"runner", "arguments", "property", "known-gaps", "baseline-policy", "ruler-paths", "timeout-ms"},
                    "oracle");
      auto result = OracleDefinition{};
      result.id = std::move(id);
      result.runner = parseOracleRunner(required(node, "runner"));
      auto arguments = required(node, "arguments");
      requireMap(arguments, "oracle arguments");

      for (auto child : arguments.children())
      {
        result.arguments.emplace(key(child), scalar(child, "oracle argument"));
      }

      result.property = scalar(required(node, "property"), "oracle property");
      result.knownGaps = stringSequence(required(node, "known-gaps"), "oracle known gaps");
      result.baselinePolicy = parseBaseline(required(node, "baseline-policy"));
      result.rulerPaths = pathSequence(required(node, "ruler-paths"), "oracle ruler paths");

      if (auto timeout = optional(node, "timeout-ms"); timeout.readable())
      {
        result.optTimeout = std::chrono::milliseconds{unsignedValue(timeout, "oracle timeout-ms")};
      }

      auto allowedArguments = std::set<std::string>{};

      if (result.runner == OracleRunner::TestCore || result.runner == OracleRunner::TestGtk)
      {
        allowedArguments.insert("filter");
      }
      else if (result.runner == OracleRunner::TidyClean)
      {
        allowedArguments.insert("scope");
      }
      else if (result.runner == OracleRunner::TestDelta)
      {
        allowedArguments.insert("test-paths");
        allowedArguments.insert("test-suffixes");
      }
      else if (result.runner == OracleRunner::PublicSignatureDelta)
      {
        allowedArguments.insert("header-prefixes");
        allowedArguments.insert("header-suffixes");
      }

      for (auto const& [name, ignored] : result.arguments)
      {
        [[maybe_unused]] auto const _ = ignored;

        if (!allowedArguments.contains(name))
        {
          throwException<ParseFailure>("oracle '{}' does not accept argument '{}'", result.id, name);
        }
      }

      return result;
    }

    Binding parseBinding(std::string taskKind, ryml::ConstNodeRef node)
    {
      rejectUnknown(node, {"agent", "engine", "oracle", "risk-oracle", "params"}, "binding");
      auto result = Binding{};
      result.taskKind = std::move(taskKind);
      result.agent = scalar(required(node, "agent"), "binding agent");
      result.engine = parseEngine(required(node, "engine"));

      if (auto oracle = optional(node, "oracle"); oracle.readable())
      {
        result.optOracle = scalar(oracle, "binding oracle");
      }

      if (auto risk = optional(node, "risk-oracle"); risk.readable())
      {
        result.optRiskOracle = scalar(risk, "binding risk oracle");
      }

      if (auto params = required(node, "params"); result.engine == EngineKind::Gate)
      {
        rejectUnknown(params, {"fanout", "top-k", "max-rounds", "churn-lines"}, "gate params");
        result.gate.fanout = unsignedValue(required(params, "fanout"), "fanout");
        result.gate.topK = unsignedValue(required(params, "top-k"), "top-k");
        result.gate.maxRounds = unsignedValue(required(params, "max-rounds"), "max-rounds");
        result.gate.churnLines = unsignedValue(required(params, "churn-lines"), "churn-lines");

        if (result.gate.fanout == 0 || result.gate.topK == 0 || result.gate.topK > result.gate.fanout ||
            result.gate.maxRounds == 0)
        {
          throwException<ParseFailure>("invalid gate fanout/top-k/max-rounds");
        }
      }
      else if (result.engine == EngineKind::Synthesis)
      {
        rejectUnknown(params, {"roster", "depth", "quorum"}, "synthesis params");
        result.synthesis.roster = stringSequence(required(params, "roster"), "synthesis roster");
        result.synthesis.depth = parseCouncilDepth(required(params, "depth"));
        result.synthesis.quorum = unsignedValue(required(params, "quorum"), "quorum");

        if (result.synthesis.quorum == 0 || result.synthesis.quorum > result.synthesis.roster.size())
        {
          throwException<ParseFailure>("invalid synthesis quorum");
        }
      }

      return result;
    }

    EscalationRule parseEscalation(ryml::ConstNodeRef node)
    {
      rejectUnknown(node, {"reason", "action", "route", "retry-limit"}, "escalation");
      auto result = EscalationRule{};
      result.reason = parseFailureReason(required(node, "reason"));
      result.action = parseEscalationAction(required(node, "action"));

      if (auto route = optional(node, "route"); route.readable())
      {
        result.optRoute = scalar(route, "escalation route");
      }

      result.retryLimit = unsignedValue(required(node, "retry-limit"), "retry-limit");

      if (result.action == EscalationAction::SwitchRoute && !result.optRoute)
      {
        throwException<ParseFailure>("switch-route escalation requires route");
      }

      return result;
    }

    void validatePlaceholders(HarnessDefinition const& harness)
    {
      auto const allowed =
        std::set<std::string>{"model", "effort", "prompt", "prompt-file", "workspace", "intent", "repo"};
      auto placeholders = std::set<std::string>{};

      for (auto const& argument : harness.argvTemplate)
      {
        auto cursor = std::size_t{};

        while ((cursor = argument.find('{', cursor)) != std::string::npos)
        {
          auto const end = argument.find('}', cursor + 1);

          if (end == std::string::npos)
          {
            throwException<ParseFailure>("unclosed placeholder in harness '{}'", harness.id);
          }

          auto const placeholder = argument.substr(cursor + 1, end - cursor - 1);

          if (!allowed.contains(placeholder))
          {
            throwException<ParseFailure>("unknown placeholder '{{{}}}' in harness '{}'", placeholder, harness.id);
          }

          placeholders.insert(placeholder);
          cursor = end + 1;
        }
      }

      // The registry's model declaration must be the single source of truth for what the CLI
      // actually runs, so every harness is forced to thread it through.
      if (!placeholders.contains("model"))
      {
        throwException<ParseFailure>("harness '{}' argv must use '{{model}}'", harness.id);
      }

      if (harness.promptDelivery == PromptDelivery::Argument && !placeholders.contains("prompt"))
      {
        throwException<ParseFailure>("harness '{}' uses argument prompt delivery without '{{prompt}}'", harness.id);
      }

      if (harness.promptDelivery == PromptDelivery::File && !placeholders.contains("prompt-file"))
      {
        throwException<ParseFailure>("harness '{}' uses file prompt delivery without '{{prompt-file}}'", harness.id);
      }
    }

    void validateRegistry(Registry const& registry)
    {
      for (auto const& [id, harness] : registry.harnesses)
      {
        requireSafeIdentifier(id, "harness id");
        validatePlaceholders(harness);
      }

      for (auto const& [id, ignored] : registry.agents)
      {
        [[maybe_unused]] auto const _ = ignored;
        requireSafeIdentifier(id, "agent id");
      }

      for (auto const& [kind, binding] : registry.bindings)
      {
        requireSafeIdentifier(kind, "binding task-kind");

        if (binding.engine == EngineKind::Search)
        {
          throwException<ParseFailure>("binding '{}' enables unsupported search engine", kind);
        }

        if (!registry.agents.contains(binding.agent))
        {
          throwException<ParseFailure>("binding '{}' references unknown agent '{}'", kind, binding.agent);
        }

        if (binding.optOracle && !registry.oracles.contains(*binding.optOracle))
        {
          throwException<ParseFailure>("binding '{}' references unknown oracle '{}'", kind, *binding.optOracle);
        }

        if (binding.optRiskOracle && !registry.oracles.contains(*binding.optRiskOracle))
        {
          throwException<ParseFailure>(
            "binding '{}' references unknown risk oracle '{}'", kind, *binding.optRiskOracle);
        }

        auto rosterMembers = std::set<std::string_view>{};
        auto rosterVendors = std::set<std::string_view>{};

        for (auto const& member : binding.synthesis.roster)
        {
          auto memberIt = registry.agents.find(member);

          if (memberIt == registry.agents.end())
          {
            throwException<ParseFailure>("binding '{}' roster references unknown agent '{}'", kind, member);
          }

          if (!rosterMembers.insert(member).second)
          {
            throwException<ParseFailure>("binding '{}' roster lists agent '{}' twice", kind, member);
          }

          rosterVendors.insert(memberIt->second.vendor);
        }

        // A same-vendor panel defeats the diversity purpose of a synthesis council (design doc
        // section 7), so vendor spread is a load-time invariant, not an advisory hope.
        if (binding.synthesis.roster.size() > 1 && rosterVendors.size() < 2)
        {
          throwException<ParseFailure>(
            "binding '{}' roster is single-vendor; councils require cross-vendor diversity", kind);
        }
      }

      for (auto const& [reason, rule] : registry.escalations)
      {
        if (rule.optRoute && !registry.agents.contains(*rule.optRoute))
        {
          throwException<ParseFailure>(
            "escalation '{}' references unknown agent '{}'", toString(reason), *rule.optRoute);
        }
      }

      for (auto const& [id, ignored] : registry.oracles)
      {
        [[maybe_unused]] auto const _ = ignored;
        requireSafeIdentifier(id, "oracle id");
      }
    }

    std::string indentBlock(std::string_view value, std::size_t spaces)
    {
      auto result = std::string{};
      auto prefix = std::string(spaces, ' ');
      auto start = std::size_t{};

      while (start <= value.size())
      {
        auto const end = value.find('\n', start);
        result += prefix;
        result.append(value.substr(start, end == std::string_view::npos ? value.size() - start : end - start));
        result += '\n';

        if (end == std::string_view::npos)
        {
          break;
        }

        start = end + 1;
      }

      return result;
    }

    std::string emitStringSequence(std::vector<std::string> const& values, std::size_t indent)
    {
      if (values.empty())
      {
        return " []\n";
      }

      auto result = std::string{"\n"};
      auto prefix = std::string(indent, ' ');

      for (auto const& value : values)
      {
        result += std::format("{}- {}\n", prefix, yamlScalar(value));
      }

      return result;
    }
  } // namespace

  Result<Registry> loadRegistry(std::filesystem::path const& path)
  {
    auto parsed = parseYaml(path);

    if (!parsed)
    {
      return std::unexpected{parsed.error()};
    }

    try
    {
      auto root = documentRoot(parsed->tree);
      rejectUnknown(
        root, {"schema", "harnesses", "agents", "oracles", "bindings", "escalations", "ruler-paths"}, "registry");

      if (scalar(required(root, "schema"), "schema") != "aobus-fleet-registry/v1")
      {
        throwException<ParseFailure>("unsupported registry schema");
      }

      auto result = Registry{};
      auto harnesses = required(root, "harnesses");
      requireMap(harnesses, "harnesses");

      for (auto child : harnesses.children())
      {
        result.harnesses.emplace(key(child), parseHarness(key(child), child));
      }

      auto agents = required(root, "agents");
      requireMap(agents, "agents");

      for (auto child : agents.children())
      {
        result.agents.emplace(key(child), parseAgent(key(child), child, result.harnesses));
      }

      auto oracles = required(root, "oracles");
      requireMap(oracles, "oracles");

      for (auto child : oracles.children())
      {
        result.oracles.emplace(key(child), parseOracle(key(child), child));
      }

      auto bindings = required(root, "bindings");
      requireMap(bindings, "bindings");

      for (auto child : bindings.children())
      {
        result.bindings.emplace(key(child), parseBinding(key(child), child));
      }

      auto escalations = required(root, "escalations");
      requireSequence(escalations, "escalations");

      for (auto child : escalations.children())
      {
        if (auto rule = parseEscalation(child); !result.escalations.emplace(rule.reason, rule).second)
        {
          throwException<ParseFailure>("duplicate escalation reason '{}'", toString(rule.reason));
        }
      }

      result.rulerPaths = pathSequence(required(root, "ruler-paths"), "registry ruler paths");
      validateRegistry(result);
      return result;
    }
    catch (std::exception const& exception)
    {
      return validationError(path.string(), exception.what());
    }
  }

  Result<PhaseIntent> loadIntent(std::filesystem::path const& path)
  {
    auto parsed = parseYaml(path);

    if (!parsed)
    {
      return std::unexpected{parsed.error()};
    }

    try
    {
      return parseIntentRoot(documentRoot(parsed->tree));
    }
    catch (std::exception const& exception)
    {
      return validationError(path.string(), exception.what());
    }
  }

  Result<std::vector<PhaseIntent>> loadIntents(std::vector<std::filesystem::path> const& paths)
  {
    auto result = std::vector<PhaseIntent>{};
    auto ids = std::set<std::string>{};

    for (auto const& path : paths)
    {
      auto intent = loadIntent(path);

      if (!intent)
      {
        return std::unexpected{intent.error()};
      }

      if (!ids.insert(intent->id).second)
      {
        return validationError(path.string(), std::format("duplicate intent id '{}'", intent->id));
      }

      result.push_back(std::move(*intent));
    }

    for (auto const& intent : result)
    {
      for (auto const& dependency : intent.dependsOn)
      {
        if (!ids.contains(dependency))
        {
          return validationError(intent.id, std::format("dangling dependency '{}'", dependency));
        }
      }
    }

    return result;
  }

  namespace
  {
    void applyBindingOverrides(Binding& binding, IntentOverrides const& overrides)
    {
      forEachOverrideField(
        [&](std::string_view name, auto member, OverridePolicy policy, auto target)
        {
          auto const& optValue = overrides.*member;

          if (!optValue)
          {
            return;
          }

          auto& slot = std::invoke(target, binding);

          if (policy == OverridePolicy::TightenUpper && *optValue > slot)
          {
            throwException<ParseFailure>("intent {} override may only tighten", name);
          }

          if (policy == OverridePolicy::TightenLower && *optValue < slot)
          {
            throwException<ParseFailure>("intent {} override may only tighten", name);
          }

          slot = *optValue;
        });
    }
  } // namespace

  Result<ResolvedPhase> resolvePhase(Registry const& registry, PhaseIntent const& intent)
  {
    try
    {
      auto bindingIt = registry.bindings.find(intent.taskKind);

      if (bindingIt == registry.bindings.end())
      {
        throwException<ParseFailure>("no binding for task-kind '{}'", intent.taskKind);
      }

      auto binding = bindingIt->second;
      applyBindingOverrides(binding, intent.overrides);

      if (binding.engine == EngineKind::Search)
      {
        throwException<ParseFailure>("search engine is not supported in production registry");
      }

      auto agentIt = registry.agents.find(binding.agent);

      if (agentIt == registry.agents.end())
      {
        throwException<ParseFailure>("unknown agent '{}'", binding.agent);
      }

      auto result = ResolvedPhase{.intent = intent,
                                  .agent = agentIt->second,
                                  .binding = binding,
                                  .optOracle = std::nullopt,
                                  .optRiskOracle = std::nullopt};

      if (binding.optOracle)
      {
        result.optOracle = registry.oracles.at(*binding.optOracle);
      }

      if (binding.optRiskOracle)
      {
        result.optRiskOracle = registry.oracles.at(*binding.optRiskOracle);
      }

      return result;
    }
    catch (std::exception const& exception)
    {
      return validationError(intent.id, exception.what());
    }
  }

  std::string yamlScalar(std::string_view value)
  {
    auto result = std::string{"\""};

    for (char const character : value)
    {
      switch (character)
      {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
          // Raw C0 control bytes are invalid inside YAML double-quoted scalars and would
          // corrupt the emitted document; agent output is untrusted, so escape them.
          if (auto const byte = static_cast<unsigned char>(character); byte < kAsciiControlBound || byte == kAsciiDel)
          {
            result += std::format("\\x{:02X}", byte);
          }
          else
          {
            result += character;
          }

          break;
      }
    }

    result += '"';
    return result;
  }

  namespace
  {
    bool hasIntentOverrides(IntentOverrides const& overrides)
    {
      auto result = false;
      forEachOverrideField([&](std::string_view /*name*/, auto member, OverridePolicy /*policy*/, auto /*target*/)
                           { result = result || static_cast<bool>(overrides.*member); });
      return result;
    }

    void emitIntentOverrides(std::ostringstream& out, IntentOverrides const& overrides)
    {
      if (!hasIntentOverrides(overrides))
      {
        out << "overrides: {}\n";
        return;
      }

      out << "overrides:\n";
      forEachOverrideField(
        [&](std::string_view name, auto member, OverridePolicy /*policy*/, auto /*target*/)
        {
          auto const& optValue = overrides.*member;

          if (!optValue)
          {
            return;
          }

          using Value = typename std::remove_cvref_t<decltype(optValue)>::value_type;
          out << "  " << name << ": ";

          if constexpr (std::same_as<Value, std::string>)
          {
            out << yamlScalar(*optValue);
          }
          else if constexpr (std::same_as<Value, std::size_t>)
          {
            out << *optValue;
          }
          else
          {
            out << toString(*optValue);
          }

          out << "\n";
        });
    }
  } // namespace

  std::string emitIntent(PhaseIntent const& intent)
  {
    auto out = std::ostringstream{};
    out << "schema: aobus-fleet-intent/v1\n";
    out << "id: " << yamlScalar(intent.id) << "\n";
    out << "task-kind: " << yamlScalar(intent.taskKind) << "\n";
    out << "invariant: " << yamlScalar(intent.invariant) << "\n";
    out << "scope:\n";

    for (auto const& rule : intent.scope)
    {
      out << "  - path: " << yamlScalar(rule.path.generic_string()) << "\n";
      out << "    operations:";

      if (rule.operations.empty())
      {
        out << " []\n";
      }
      else
      {
        out << "\n";

        for (auto operation : rule.operations)
        {
          out << "      - " << toString(operation) << "\n";
        }
      }
    }

    out << "depends-on:" << emitStringSequence(intent.dependsOn, 2);
    emitIntentOverrides(out, intent.overrides);
    out << "body: |\n" << indentBlock(intent.body, 2);
    return out.str();
  }

  std::string emitResolved(ResolvedPhase const& phase)
  {
    auto out = std::ostringstream{};
    out << "schema: aobus-fleet-resolved/v1\n";
    out << "phase-id: " << yamlScalar(phase.intent.id) << "\n";
    out << "task-kind: " << yamlScalar(phase.intent.taskKind) << "\n";
    out << "engine: " << toString(phase.binding.engine) << "\n";
    out << "agent: " << yamlScalar(phase.agent.id) << "\n";
    out << "harness: " << yamlScalar(phase.agent.harness) << "\n";
    out << "model: " << yamlScalar(phase.agent.model) << "\n";
    out << "vendor: " << yamlScalar(phase.agent.vendor) << "\n";
    out << "effort: " << (phase.agent.effort.empty() ? std::string{"null"} : yamlScalar(phase.agent.effort)) << "\n";
    out << "oracle: " << (phase.optOracle ? yamlScalar(phase.optOracle->id) : "null") << "\n";
    out << "risk-oracle: " << (phase.optRiskOracle ? yamlScalar(phase.optRiskOracle->id) : "null") << "\n";
    return out.str();
  }

  std::string emitManifest(ReviewManifest const& manifest)
  {
    auto out = std::ostringstream{};
    out << "schema: aobus-fleet-manifest/v1\n";
    out << "phase-id: " << yamlScalar(manifest.phaseId) << "\n";
    out << "output-mode: " << toString(manifest.mode) << "\n";
    out << "failure: " << toString(manifest.failure) << "\n";
    out << "escalation-action: " << (manifest.optEscalationAction ? toString(*manifest.optEscalationAction) : "none")
        << "\n";
    out << "summary: " << yamlScalar(manifest.summary) << "\n";
    out << "route-key: " << yamlScalar(manifest.route.canonical()) << "\n";
    out << "patch:\n";
    out << "  present: " << (manifest.optPatch ? "true" : "false") << "\n";

    if (manifest.optPatch)
    {
      out << "  candidate-id: " << yamlScalar(manifest.optPatch->candidateId) << "\n";
      out << "  touched-file-count: " << manifest.optPatch->touchedFiles.size() << "\n";
      out << "  added-line-count: " << manifest.optPatch->addedLines << "\n";
      out << "  removed-line-count: " << manifest.optPatch->removedLines << "\n";
    }

    return out.str();
  }

  std::string emitEvidence(ReviewManifest const& manifest)
  {
    auto out = std::ostringstream{};
    out << "schema: aobus-fleet-evidence/v1\n";
    out << "phase-id: " << yamlScalar(manifest.phaseId) << "\n";
    out << "oracles:\n";

    for (auto const& evidence : manifest.oracleEvidence)
    {
      out << "  - id: " << yamlScalar(evidence.oracleId) << "\n";
      out << "    version: " << yamlScalar(evidence.oracleVersion) << "\n";
      out << "    property: " << yamlScalar(evidence.property) << "\n";
      out << "    passed: " << (evidence.passed ? "true" : "false") << "\n";
      out << "    infrastructure-error: " << (evidence.infrastructureError ? "true" : "false") << "\n";
      out << "    exit-code: " << evidence.exitCode << "\n";
      constexpr auto kGapsIndent = std::size_t{6};
      out << "    known-gaps:" << emitStringSequence(evidence.knownGaps, kGapsIndent);
    }

    out << "risks:\n";

    for (auto const& risk : manifest.riskEvidence)
    {
      out << "  - id: " << yamlScalar(risk.oracleId) << "\n";
      out << "    fired: " << (risk.fired ? "true" : "false") << "\n";
      out << "    detail: " << yamlScalar(risk.detail) << "\n";
    }

    return out.str();
  }

  std::string emitTraceEvent(std::string_view event, std::map<std::string, std::string, std::less<>> const& fields)
  {
    auto out = std::ostringstream{};
    out << "schema: aobus-fleet-trace-event/v1\n";
    out << "event: " << yamlScalar(event) << "\n";
    out << "timestamp: " << yamlScalar(utcTimestamp()) << "\n";

    for (auto const& [name, value] : fields)
    {
      out << name << ": " << yamlScalar(value) << "\n";
    }

    return out.str();
  }

  Result<> appendYamlDocument(std::filesystem::path const& path, std::string_view document)
  {
    auto directoryError = std::error_code{};

    if (!path.parent_path().empty())
    {
      std::filesystem::create_directories(path.parent_path(), directoryError);
    }

    if (directoryError)
    {
      return validationError(Error::Code::IoError, path.string(), directoryError.message());
    }

    auto payload = std::string{"---\n"};
    payload.append(document);

    if (payload.back() != '\n')
    {
      payload += '\n';
    }

    auto file = std::ofstream{path, std::ios::app};

    if (!file.is_open())
    {
      return validationError(Error::Code::IoError, path.string(), "cannot open file for append");
    }

    file << payload;

    if (file.fail())
    {
      return validationError(Error::Code::IoError, path.string(), "short append write");
    }

    return {};
  }

  Result<StreamReadResult> readReviewOutcomes(std::filesystem::path const& path)
  {
    auto stream = readScalarStream(path, "aobus-fleet-review-outcome/v1");

    if (!stream)
    {
      return std::unexpected{stream.error()};
    }

    static constexpr auto kAllowed =
      std::to_array<std::string_view>({"schema", "event", "phase-id", "route-key", "verdict", "reason", "timestamp"});
    auto result = StreamReadResult{.outcomes = {}, .trailingCorruption = stream->trailingCorruption};

    for (auto const& document : stream->documents)
    {
      for (auto const& name : std::views::keys(document))
      {
        if (!std::ranges::contains(kAllowed, name))
        {
          return validationError(path.string(), std::format("unknown field '{}' in review outcome", name));
        }
      }

      auto field = [&](std::string_view name) -> std::string const*
      {
        auto const found = document.find(name);
        return found == document.end() ? nullptr : &found->second;
      };

      auto const* const phaseId = field("phase-id");
      auto const* const route = field("route-key");
      auto const* const verdictText = field("verdict");
      auto const* const reason = field("reason");
      auto const* const timestamp = field("timestamp");

      if (phaseId == nullptr || route == nullptr || verdictText == nullptr || reason == nullptr || timestamp == nullptr)
      {
        return validationError(path.string(), "review outcome is missing a required field");
      }

      auto const optVerdict = parseReviewVerdict(*verdictText);

      if (!optVerdict)
      {
        return validationError(path.string(), std::format("invalid review verdict '{}'", *verdictText));
      }

      result.outcomes.push_back(ReviewOutcome{
        .phaseId = *phaseId,
        .route = *route,
        .verdict = *optVerdict,
        .reason = *reason,
        .timestamp = *timestamp,
      });
    }

    return result;
  }

  Result<ScalarStreamResult> readScalarStream(std::filesystem::path const& path, std::string_view schema)
  {
    if (!std::filesystem::exists(path))
    {
      return ScalarStreamResult{};
    }

    auto source = readDocument(path);

    if (!source)
    {
      return std::unexpected{source.error()};
    }

    auto result = ScalarStreamResult{};
    auto cursor = std::size_t{};

    while (cursor < source->size())
    {
      auto start = source->find("---\n", cursor);

      if (start == std::string::npos)
      {
        break;
      }

      start += 4;
      auto const next = source->find("---\n", start);
      auto const complete = next != std::string::npos || (!source->empty() && source->back() == '\n');

      if (!complete)
      {
        result.trailingCorruption = true;
        break;
      }

      auto const document = source->substr(start, next == std::string::npos ? source->size() - start : next - start);

      try
      {
        auto tree = ryml::Tree{yaml::callbacks()};
        ryml::parse_in_arena(yaml::toCsubstr(document), &tree);
        auto entries = std::size_t{};
        validateTree(tree.rootref(), 0, entries);
        auto root = documentRoot(tree);
        requireMap(root, "stream document");
        auto values = std::map<std::string, std::string, std::less<>>{};

        for (auto child : root.children())
        {
          values.emplace(key(child), scalar(child, "stream scalar"));
        }

        if (auto schemaIt = values.find("schema"); schemaIt == values.end() || schemaIt->second != schema)
        {
          throwException<ParseFailure>("unexpected stream schema");
        }

        result.documents.push_back(std::move(values));
      }
      catch (std::exception const& exception)
      {
        if (next == std::string::npos)
        {
          result.trailingCorruption = true;
          break;
        }

        return validationError(path.string(), exception.what());
      }

      if (next == std::string::npos)
      {
        break;
      }

      cursor = next;
    }

    return result;
  }

  Result<std::string> loadManifestRoute(std::filesystem::path const& path)
  {
    auto parsed = parseYaml(path);

    if (!parsed)
    {
      return std::unexpected{parsed.error()};
    }

    try
    {
      auto root = documentRoot(parsed->tree);
      requireMap(root, "manifest");

      if (scalar(required(root, "schema"), "schema") != "aobus-fleet-manifest/v1")
      {
        throwException<ParseFailure>("unsupported manifest schema");
      }

      return scalar(required(root, "route-key"), "route-key");
    }
    catch (std::exception const& exception)
    {
      return validationError(path.string(), exception.what());
    }
  }

  std::string utcTimestamp()
  {
    auto const now = std::chrono::system_clock::now();
    auto const secondsInstant = std::chrono::floor<std::chrono::seconds>(now);
    return std::format("{:%FT%TZ}", secondsInstant);
  }

  std::optional<ReviewVerdict> parseReviewVerdict(std::string_view value)
  {
    return enumFromName(kReviewVerdictNames, value);
  }
} // namespace ao::fleet
