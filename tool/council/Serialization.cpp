// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "council/Serialization.h"

#include "council/CouncilSchema.h"
#include "council/YamlEmit.h"
#include <ao/Error.h>
#include <ao/Exception.h>
#include <ao/yaml/RymlAdapter.h>

#include <ryml.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iomanip>
#include <ios>
#include <map>
#include <mutex>
#include <optional>
#include <print>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ao::council
{
  namespace
  {
    constexpr auto kMaxDocumentBytes = std::size_t{2UL * 1024 * 1024};
    constexpr auto kMaxDepth = std::size_t{32};
    constexpr auto kMaxCollectionEntries = std::size_t{20'000};
    constexpr auto kIntentSchema = std::string_view{"aobus-council-intent/v1"};
    constexpr auto kRegistrySchema = std::string_view{"aobus-council-registry/v1"};

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
          if (auto const childKey = std::string{yaml::keyView(child)}; !keys.insert(childKey).second)
          {
            throwException<ParseFailure>("duplicate key '{}'", childKey);
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

    Result<ParsedYaml> parseYamlSource(std::string source, std::string context)
    {
      try
      {
        auto yamlContext = yaml::CallbackContext{context};
        auto parsed = ParsedYaml{};
        parsed.source = std::move(source);
        yaml::parseInArena(parsed.tree, parsed.source, yamlContext);
        std::size_t entries = 0;
        validateTree(parsed.tree.rootref(), 0, entries);
        parsed.tree.callbacks(yaml::callbacks());
        return parsed;
      }
      catch (std::exception const& exception)
      {
        return validationError(std::move(context), exception.what());
      }
    }

    Result<ParsedYaml> parseYaml(std::filesystem::path const& path)
    {
      auto source = readDocument(path);

      if (!source)
      {
        return std::unexpected{source.error()};
      }

      return parseYamlSource(std::move(*source), path.string());
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

    std::optional<unsigned char> hexByte(char high, char low)
    {
      auto const value = [](char character) -> std::optional<unsigned char>
      {
        static constexpr unsigned char kHexAlphaOffset = 10;

        if (character >= '0' && character <= '9')
        {
          return static_cast<unsigned char>(character - '0');
        }

        if (character >= 'a' && character <= 'f')
        {
          return static_cast<unsigned char>(character - 'a' + kHexAlphaOffset);
        }

        if (character >= 'A' && character <= 'F')
        {
          return static_cast<unsigned char>(character - 'A' + kHexAlphaOffset);
        }

        return std::nullopt;
      };

      auto const optHighValue = value(high);
      auto const optLowValue = value(low);

      if (!optHighValue || !optLowValue)
      {
        return std::nullopt;
      }

      return static_cast<unsigned char>((*optHighValue << 4U) | *optLowValue);
    }

    std::string decodeByteEscapes(std::string_view value)
    {
      auto result = std::string{};

      for (std::size_t index = 0; index < value.size(); ++index)
      {
        if (index + 3 < value.size() && value[index] == '\\' && value[index + 1] == 'x')
        {
          if (auto const optByte = hexByte(value[index + 2], value[index + 3]); optByte)
          {
            result.push_back(static_cast<char>(*optByte));
            index += 3;
            continue;
          }
        }

        result.push_back(value[index]);
      }

      return result;
    }

    std::string streamScalar(ryml::ConstNodeRef node, std::string_view context)
    {
      return decodeByteEscapes(scalar(node, context));
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

    void rejectUnknown(ryml::ConstNodeRef node,
                       std::initializer_list<std::string_view> allowed,
                       std::string_view context)
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

    std::size_t unsignedValue(ryml::ConstNodeRef node, std::string_view context)
    {
      auto const text = scalar(node, context);
      std::size_t result = 0;
      auto const conversion = std::from_chars(text.data(), text.data() + text.size(), result);

      if (conversion.ec != std::errc{} || conversion.ptr != text.data() + text.size())
      {
        throwException<ParseFailure>("{} must be an unsigned integer", context);
      }

      return result;
    }

    std::chrono::milliseconds millisecondsDuration(ryml::ConstNodeRef node, std::string_view context)
    {
      return std::chrono::milliseconds{unsignedValue(node, context)};
    }

    PromptDelivery parsePromptDelivery(ryml::ConstNodeRef node)
    {
      return closedEnum(node, "prompt delivery", kPromptDeliveryNames);
    }

    Depth parseDepth(ryml::ConstNodeRef node)
    {
      return closedEnum(node, "depth", kDepthNames);
    }

    std::vector<std::string> stringSequence(ryml::ConstNodeRef node, std::string_view context)
    {
      requireSequence(node, context);
      auto result = std::vector<std::string>{};

      for (auto child : node.children())
      {
        result.push_back(scalar(child, context));
      }

      return result;
    }

    std::filesystem::path safeRelativePath(std::string value, std::string_view context)
    {
      if (value.ends_with('/'))
      {
        value.pop_back();
      }

      auto path = std::filesystem::path{value}.lexically_normal();

      if (path.empty() || path.is_absolute())
      {
        throwException<ParseFailure>("{} must be a non-empty relative path", context);
      }

      for (auto const& part : path)
      {
        if (part == "..")
        {
          throwException<ParseFailure>("{} must not traverse outside the repository", context);
        }
      }

      return path;
    }

    void requireSafeIdentifier(std::string_view value, std::string_view context)
    {
      if (value.empty())
      {
        throwException<ParseFailure>("{} must not be empty", context);
      }

      if (value == "." || value == "..")
      {
        throwException<ParseFailure>("{} '{}' is reserved", context, value);
      }

      for (auto const character : value)
      {
        auto const valid = std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '-' ||
                           character == '_' || character == '.';

        if (!valid)
        {
          throwException<ParseFailure>("{} '{}' contains an invalid character", context, value);
        }
      }
    }

    void replaceAll(std::string& value, std::string_view needle, std::string_view replacement)
    {
      std::size_t position = 0;

      while ((position = value.find(needle, position)) != std::string::npos)
      {
        value.replace(position, needle.size(), replacement);
        position += replacement.size();
      }
    }

    bool hasUnresolvedPlaceholder(std::string argument, bool allowPromptPlaceholders)
    {
      if (allowPromptPlaceholders)
      {
        replaceAll(argument, "{prompt}", "");
        replaceAll(argument, "{prompt-file}", "");
      }

      return argument.find('{') != std::string::npos || argument.find('}') != std::string::npos;
    }

    void requireNoPlaceholders(std::vector<std::string> const& argv,
                               std::string_view context,
                               bool allowPromptPlaceholders = false)
    {
      for (auto const& argument : argv)
      {
        if (hasUnresolvedPlaceholder(argument, allowPromptPlaceholders))
        {
          throwException<ParseFailure>("{} leaves unresolved argv placeholder '{}'", context, argument);
        }
      }
    }

    std::vector<std::string> materializeArgv(std::vector<std::string> argv, AgentDefinition const& agent)
    {
      for (auto& argument : argv)
      {
        if (argument.find("{effort}") != std::string::npos && agent.effort.empty())
        {
          throwException<ParseFailure>("agent '{}' argv uses '{{effort}}' but effort is empty", agent.id);
        }

        replaceAll(argument, "{model}", agent.model);
        replaceAll(argument, "{effort}", agent.effort);
      }

      requireNoPlaceholders(argv, std::format("agent '{}'", agent.id), true);
      return argv;
    }

    HarnessDefinition parseHarness(std::string id, ryml::ConstNodeRef node)
    {
      rejectUnknown(node, {"argv", "prompt-delivery", "environment-whitelist", "timeout-ms"}, "harness");

      auto result = HarnessDefinition{};
      result.id = std::move(id);
      requireSafeIdentifier(result.id, "harness id");
      result.argvTemplate = stringSequence(required(node, "argv"), "harness argv");

      if (result.argvTemplate.empty())
      {
        throwException<ParseFailure>("harness '{}' argv must not be empty", result.id);
      }

      if (auto prompt = optional(node, "prompt-delivery"); prompt.readable())
      {
        result.promptDelivery = parsePromptDelivery(prompt);
      }

      if (auto environment = optional(node, "environment-whitelist"); environment.readable())
      {
        result.environmentWhitelist = stringSequence(environment, "harness environment whitelist");
      }

      if (auto timeout = optional(node, "timeout-ms"); timeout.readable())
      {
        result.timeout = millisecondsDuration(timeout, "harness timeout-ms");
      }

      return result;
    }

    AgentDefinition parseAgent(std::string id,
                               ryml::ConstNodeRef node,
                               std::map<std::string, HarnessDefinition, std::less<>> const& harnesses)
    {
      rejectUnknown(
        node,
        {"harness", "model", "vendor", "effort", "argv", "prompt-delivery", "environment-whitelist", "timeout-ms"},
        "agent");

      auto result = AgentDefinition{};
      result.id = std::move(id);
      requireSafeIdentifier(result.id, "agent id");
      result.harness = scalar(required(node, "harness"), "agent harness");
      auto harnessIt = harnesses.find(result.harness);

      if (harnessIt == harnesses.end())
      {
        throwException<ParseFailure>("agent '{}' references unknown harness '{}'", result.id, result.harness);
      }

      result.model = scalar(required(node, "model"), "agent model");
      result.vendor = scalar(required(node, "vendor"), "agent vendor");

      if (result.model.empty() || result.vendor.empty())
      {
        throwException<ParseFailure>("agent '{}' model and vendor must not be empty", result.id);
      }

      if (auto effort = optional(node, "effort"); effort.readable())
      {
        result.effort = scalar(effort, "agent effort");
      }

      auto const& harness = harnessIt->second;
      auto argv =
        optional(node, "argv").readable() ? stringSequence(optional(node, "argv"), "agent argv") : harness.argvTemplate;
      result.argvTemplate = materializeArgv(std::move(argv), result);
      result.promptDelivery = optional(node, "prompt-delivery").readable()
                                ? parsePromptDelivery(optional(node, "prompt-delivery"))
                                : harness.promptDelivery;
      result.environmentWhitelist =
        optional(node, "environment-whitelist").readable()
          ? stringSequence(optional(node, "environment-whitelist"), "agent environment whitelist")
          : harness.environmentWhitelist;
      result.timeout = optional(node, "timeout-ms").readable()
                         ? millisecondsDuration(optional(node, "timeout-ms"), "agent timeout-ms")
                         : harness.timeout;

      if (result.argvTemplate.empty())
      {
        throwException<ParseFailure>("agent '{}' argv must not be empty", result.id);
      }

      return result;
    }

    Definition parseDefinition(std::string id, ryml::ConstNodeRef node)
    {
      rejectUnknown(node, {"roster", "depth", "quorum"}, "definition");

      auto result = Definition{};
      result.taskKind = std::move(id);
      requireSafeIdentifier(result.taskKind, "task-kind");
      result.parameters.roster = stringSequence(required(node, "roster"), "roster");

      if (auto depth = optional(node, "depth"); depth.readable())
      {
        result.parameters.depth = parseDepth(depth);
      }

      if (auto quorum = optional(node, "quorum"); quorum.readable())
      {
        result.parameters.quorum = unsignedValue(quorum, "quorum");
      }

      return result;
    }

    void validateDefinition(Registry const& registry, Definition const& definition, std::string_view context)
    {
      if (definition.parameters.roster.empty())
      {
        throwException<ParseFailure>("{} roster must not be empty", context);
      }

      if (definition.parameters.quorum == 0 || definition.parameters.quorum > definition.parameters.roster.size())
      {
        throwException<ParseFailure>("{} quorum must be between 1 and the roster size", context);
      }

      auto agents = std::set<std::string>{};
      auto vendors = std::set<std::string>{};

      for (auto const& agentId : definition.parameters.roster)
      {
        if (!agents.insert(agentId).second)
        {
          throwException<ParseFailure>("{} roster repeats agent '{}'", context, agentId);
        }

        auto agentIt = registry.agents.find(agentId);

        if (agentIt == registry.agents.end())
        {
          throwException<ParseFailure>("{} roster references unknown agent '{}'", context, agentId);
        }

        if (auto const& agent = agentIt->second; !agent.vendor.empty() && !vendors.insert(agent.vendor).second)
        {
          throwException<ParseFailure>("{} roster repeats vendor '{}'", context, agent.vendor);
        }
      }
    }

    void validateRegistry(Registry const& registry)
    {
      for (auto const& [id, harness] : registry.harnesses)
      {
        requireSafeIdentifier(id, "harness id");

        if (harness.argvTemplate.empty())
        {
          throwException<ParseFailure>("harness '{}' argv must not be empty", id);
        }
      }

      for (auto const& [id, agent] : registry.agents)
      {
        requireSafeIdentifier(id, "agent id");

        if (!registry.harnesses.contains(agent.harness))
        {
          throwException<ParseFailure>("agent '{}' references unknown harness '{}'", id, agent.harness);
        }
      }

      for (auto const& [id, definition] : registry.councils)
      {
        validateDefinition(registry, definition, std::format("definition '{}'", id));
      }
    }

    FocusRule parseFocusRule(ryml::ConstNodeRef node)
    {
      rejectUnknown(node, {"path", "match"}, "focus row");

      auto rule = FocusRule{};
      auto rawPath = scalar(required(node, "path"), "focus path");
      auto const prefixBySlash = rawPath.ends_with('/');
      rule.path = safeRelativePath(std::move(rawPath), "focus path");
      rule.match = prefixBySlash ? FocusMatch::Prefix : FocusMatch::Exact;

      if (auto match = optional(node, "match"); match.readable())
      {
        if (auto const value = scalar(match, "focus match"); value == "prefix")
        {
          rule.match = FocusMatch::Prefix;
        }
        else if (value == "exact")
        {
          rule.match = FocusMatch::Exact;
        }
        else
        {
          throwException<ParseFailure>("invalid focus match '{}'", value);
        }
      }

      return rule;
    }

    IntentOverrides parseOverrides(ryml::ConstNodeRef node)
    {
      rejectUnknown(node, {"roster", "depth", "quorum"}, "overrides");

      auto result = IntentOverrides{};

      if (auto roster = optional(node, "roster"); roster.readable())
      {
        result.optRoster = stringSequence(roster, "override roster");
      }

      if (auto depth = optional(node, "depth"); depth.readable())
      {
        result.optDepth = parseDepth(depth);
      }

      if (auto quorum = optional(node, "quorum"); quorum.readable())
      {
        result.optQuorum = unsignedValue(quorum, "override quorum");
      }

      return result;
    }

    PhaseIntent parseIntentRoot(ryml::ConstNodeRef root)
    {
      rejectUnknown(
        root, {"schema", "id", "task-kind", "invariant", "focus", "depends-on", "overrides", "body"}, "intent");

      if (scalar(required(root, "schema"), "schema") != kIntentSchema)
      {
        throwException<ParseFailure>("invalid intent schema");
      }

      auto result = PhaseIntent{};
      result.id = scalar(required(root, "id"), "intent id");
      result.taskKind = scalar(required(root, "task-kind"), "intent task-kind");
      result.invariant = scalar(required(root, "invariant"), "intent invariant");
      result.body = scalar(required(root, "body"), "intent body");
      requireSafeIdentifier(result.id, "intent id");
      requireSafeIdentifier(result.taskKind, "intent task-kind");

      if (auto focus = optional(root, "focus"); focus.readable())
      {
        requireSequence(focus, "focus");

        if (focus.num_children() == 0)
        {
          throwException<ParseFailure>("focus must not be empty; omit it when there is no useful hint");
        }

        auto paths = std::set<std::filesystem::path>{};

        for (auto row : focus.children())
        {
          auto rule = parseFocusRule(row);

          if (!paths.insert(rule.path).second)
          {
            throwException<ParseFailure>("duplicate focus path '{}'", rule.path.generic_string());
          }

          result.focus.push_back(std::move(rule));
        }
      }

      if (auto depends = optional(root, "depends-on"); depends.readable())
      {
        result.dependsOn = stringSequence(depends, "depends-on");
      }

      if (auto overrides = optional(root, "overrides"); overrides.readable())
      {
        result.overrides = parseOverrides(overrides);
      }

      return result;
    }

    Registry parseRegistryRoot(ryml::ConstNodeRef root)
    {
      rejectUnknown(root, {"schema", "harnesses", "agents", "councils"}, "registry");

      if (scalar(required(root, "schema"), "schema") != kRegistrySchema)
      {
        throwException<ParseFailure>("invalid registry schema");
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

      auto councils = required(root, "councils");
      requireMap(councils, "councils");

      for (auto child : councils.children())
      {
        result.councils.emplace(key(child), parseDefinition(key(child), child));
      }

      validateRegistry(result);
      return result;
    }

    std::vector<std::string> streamChunks(std::string_view source)
    {
      auto result = std::vector<std::string>{};
      auto current = std::string{};
      auto input = std::istringstream{std::string{source}};
      auto line = std::string{};

      while (std::getline(input, line))
      {
        auto lineView = std::string_view{line};

        if (!lineView.empty() && lineView.back() == '\r')
        {
          lineView.remove_suffix(1);
        }

        if (lineView == "---")
        {
          if (!current.empty())
          {
            result.push_back(std::move(current));
            current.clear();
          }

          continue;
        }

        current += line;
        current += '\n';
      }

      if (!current.empty())
      {
        result.push_back(std::move(current));
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
      return parseRegistryRoot(documentRoot(parsed->tree));
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

    for (auto const& path : paths)
    {
      auto intent = loadIntent(path);

      if (!intent)
      {
        return std::unexpected{intent.error()};
      }

      result.push_back(std::move(*intent));
    }

    return result;
  }

  Result<ResolvedPhase> resolvePhase(Registry const& registry, PhaseIntent const& intent)
  {
    try
    {
      auto definitionIt = registry.councils.find(intent.taskKind);

      if (definitionIt == registry.councils.end())
      {
        throwException<ParseFailure>("no definition for task-kind '{}'", intent.taskKind);
      }

      auto definition = definitionIt->second;

      if (intent.overrides.optRoster)
      {
        definition.parameters.roster = *intent.overrides.optRoster;
      }

      if (intent.overrides.optDepth)
      {
        definition.parameters.depth = *intent.overrides.optDepth;
      }

      if (intent.overrides.optQuorum)
      {
        definition.parameters.quorum = *intent.overrides.optQuorum;
      }

      validateDefinition(registry, definition, "resolved definition");
      return ResolvedPhase{.intent = intent, .definition = std::move(definition)};
    }
    catch (std::exception const& exception)
    {
      return validationError(Error::Code::InvalidInput, "resolve", exception.what());
    }
  }

  std::string yamlScalar(std::string_view value)
  {
    return yaml_emit::scalar(value);
  }

  std::string emitIntent(PhaseIntent const& intent)
  {
    auto out = std::ostringstream{};
    std::println(out, "schema: {}", kIntentSchema);
    std::println(out, "id: {}", yamlScalar(intent.id));
    std::println(out, "task-kind: {}", yamlScalar(intent.taskKind));
    std::println(out, "invariant: {}", yamlScalar(intent.invariant));

    if (!intent.focus.empty())
    {
      std::print(out, "focus:\n");

      for (auto const& rule : intent.focus)
      {
        std::println(out, "  - path: {}", yamlScalar(rule.path.generic_string()));
        std::println(out, "    match: {}", rule.match == FocusMatch::Prefix ? "prefix" : "exact");
      }
    }

    std::print(out, "depends-on: [");

    for (std::size_t index = 0; index < intent.dependsOn.size(); ++index)
    {
      std::print(out, "{}{}", index == 0 ? "" : ", ", yamlScalar(intent.dependsOn[index]));
    }

    std::print(out, "]\n");

    if (intent.overrides.optRoster || intent.overrides.optDepth || intent.overrides.optQuorum)
    {
      std::print(out, "overrides:\n");

      if (intent.overrides.optRoster)
      {
        std::print(out, "  roster: [");

        for (std::size_t index = 0; index < intent.overrides.optRoster->size(); ++index)
        {
          std::print(out, "{}{}", index == 0 ? "" : ", ", yamlScalar((*intent.overrides.optRoster)[index]));
        }

        std::print(out, "]\n");
      }

      if (intent.overrides.optDepth)
      {
        std::println(out, "  depth: {}", toString(*intent.overrides.optDepth));
      }

      if (intent.overrides.optQuorum)
      {
        std::println(out, "  quorum: {}", *intent.overrides.optQuorum);
      }
    }
    else
    {
      std::print(out, "overrides: {{}}\n");
    }

    std::print(out, "body: |\n");
    auto body = std::istringstream{intent.body};
    auto line = std::string{};

    while (std::getline(body, line))
    {
      std::println(out, "  {}", line);
    }

    if (intent.body.empty() || intent.body.ends_with('\n'))
    {
      std::print(out, "\n");
    }

    return out.str();
  }

  std::string emitResolved(ResolvedPhase const& phase)
  {
    auto out = std::ostringstream{};
    yaml_emit::scalarField(out, 0, "schema", "aobus-council-resolved/v1");
    yaml_emit::scalarField(out, 0, "phase-id", phase.intent.id);
    yaml_emit::scalarField(out, 0, "task-kind", phase.intent.taskKind);
    yaml_emit::beginMapField(out, 0, "council");
    yaml_emit::scalarField(out, 2, "depth", toString(phase.definition.parameters.depth));
    yaml_emit::scalarField(out, 2, "quorum", phase.definition.parameters.quorum);
    yaml_emit::flowStringSequenceField(out, 2, "roster", phase.definition.parameters.roster);
    return out.str();
  }

  std::string emitManifest(ReviewManifest const& manifest)
  {
    auto out = std::ostringstream{};
    yaml_emit::scalarField(out, 0, "schema", "aobus-council-manifest/v1");
    yaml_emit::scalarField(out, 0, "phase-id", manifest.phaseId);
    yaml_emit::scalarField(out, 0, "failure", toString(manifest.failure));
    yaml_emit::scalarField(out, 0, "summary", manifest.summary);
    return out.str();
  }

  std::string emitTraceEvent(std::string_view event, std::map<std::string, std::string, std::less<>> const& fields)
  {
    auto out = std::ostringstream{};
    yaml_emit::scalarField(out, 0, "schema", "aobus-council-trace-event/v1");
    yaml_emit::scalarField(out, 0, "event", event);
    yaml_emit::scalarField(out, 0, "timestamp", utcTimestamp());

    for (auto const& [name, value] : fields)
    {
      yaml_emit::scalarField(out, 0, name, value);
    }

    return out.str();
  }

  Result<> appendYamlDocument(std::filesystem::path const& path, std::string_view document)
  {
    static auto appendMutex = std::mutex{};
    auto const lock = std::scoped_lock{appendMutex};

    auto error = std::error_code{};

    if (!path.parent_path().empty())
    {
      std::filesystem::create_directories(path.parent_path(), error);
    }

    if (error)
    {
      return makeError(Error::Code::IoError, error.message());
    }

    auto const exists = std::filesystem::exists(path, error);

    if (error)
    {
      return makeError(Error::Code::IoError, error.message());
    }

    bool needsSeparator = false;

    if (exists)
    {
      auto const size = std::filesystem::file_size(path, error);

      if (error)
      {
        return makeError(Error::Code::IoError, error.message());
      }

      needsSeparator = size > 0;
    }

    auto output = std::ofstream{path, std::ios::binary | std::ios::app};

    if (!output.is_open())
    {
      return makeError(Error::Code::IoError, std::format("cannot append {}", path.string()));
    }

    if (needsSeparator)
    {
      std::print(output, "---\n");
    }

    output.write(document.data(), static_cast<std::streamsize>(document.size()));

    if (!document.ends_with('\n'))
    {
      std::print(output, "\n");
    }

    if (!output)
    {
      return makeError(Error::Code::IoError, std::format("cannot append {}", path.string()));
    }

    output.flush();

    if (!output)
    {
      return makeError(Error::Code::IoError, std::format("cannot flush {}", path.string()));
    }

    output.close();

    if (!output)
    {
      return makeError(Error::Code::IoError, std::format("cannot close {}", path.string()));
    }

    return {};
  }

  Result<ScalarStreamResult> readScalarStream(std::filesystem::path const& path, std::string_view schema)
  {
    auto source = readDocument(path);

    if (!source)
    {
      return std::unexpected{source.error()};
    }

    auto result = ScalarStreamResult{};
    auto const terminated = source->empty() || source->ends_with('\n');
    auto chunks = streamChunks(*source);

    for (std::size_t index = 0; index < chunks.size(); ++index)
    {
      if (index + 1 == chunks.size() && !terminated)
      {
        result.trailingCorruption = true;
        break;
      }

      auto parsed = parseYamlSource(std::move(chunks[index]), std::format("{} document {}", path.string(), index));

      if (!parsed)
      {
        if (index + 1 == chunks.size() && !terminated)
        {
          result.trailingCorruption = true;
          break;
        }

        return std::unexpected{parsed.error()};
      }

      try
      {
        auto root = documentRoot(parsed->tree);
        requireMap(root, "stream document");

        if (scalar(required(root, "schema"), "schema") != schema)
        {
          throwException<ParseFailure>("unexpected stream schema");
        }

        auto row = std::map<std::string, std::string, std::less<>>{};

        for (auto child : root.children())
        {
          if (child.is_container())
          {
            throwException<ParseFailure>("stream field '{}' must be a scalar", key(child));
          }

          row.emplace(key(child), streamScalar(child, key(child)));
        }

        result.documents.push_back(std::move(row));
      }
      catch (std::exception const& exception)
      {
        if (index + 1 == chunks.size() && !terminated)
        {
          result.trailingCorruption = true;
          break;
        }

        return validationError(path.string(), exception.what());
      }
    }

    return result;
  }

  std::string utcTimestamp()
  {
    auto const now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    auto calendar = std::tm{};
    ::gmtime_r(&now, &calendar);
    auto out = std::ostringstream{};
    out << std::put_time(&calendar, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
  }
} // namespace ao::council
