// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/yaml/RymlAdapter.h>
#include <ao/yaml/Serialization.h>

#include <c4/format.hpp>
#include <ryml.hpp>

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::yaml
{
  std::string fieldContext(std::string_view context, std::string_view field)
  {
    auto result = std::string{};
    result.reserve(kMaximumErrorContextBytes);
    bool truncated = false;

    auto const append = [&result, &truncated](std::string_view part)
    {
      auto const remaining = kMaximumErrorContextBytes - result.size();
      auto const count = std::min(remaining, part.size());
      result.append(part.substr(0, count));
      truncated = truncated || count != part.size();
    };

    append(context);

    if (!context.empty())
    {
      if (result.size() < kMaximumErrorContextBytes)
      {
        result.push_back('.');
      }
      else
      {
        truncated = true;
      }
    }

    append(field);

    if (truncated)
    {
      constexpr auto kSuffix = std::string_view{"..."};
      result.resize(kMaximumErrorContextBytes - kSuffix.size());
      result.append(kSuffix);
    }

    return result;
  }

  Result<> requireMap(ryml::ConstNodeRef node, std::string_view context)
  {
    if (!node.is_map())
    {
      return makeError(Error::Code::FormatRejected, boundedErrorContext(context) + " must be a mapping");
    }

    return {};
  }

  Result<> requireSequence(ryml::ConstNodeRef node, std::string_view context)
  {
    if (!node.is_seq())
    {
      return makeError(Error::Code::FormatRejected, boundedErrorContext(context) + " must be a sequence");
    }

    return {};
  }

  Result<> validateMapKeys(ryml::ConstNodeRef node,
                           std::span<std::string_view const> allowedKeys,
                           std::string_view context,
                           UnknownKeyPolicy unknownKeyPolicy)
  {
    if (auto const result = requireMap(node, context); !result)
    {
      return result;
    }

    auto seenKeys = std::vector<std::string_view>{};
    seenKeys.reserve(node.num_children());

    for (auto const& child : node.children())
    {
      if (!child.has_key())
      {
        return makeError(
          Error::Code::FormatRejected, boundedErrorContext(context) + " contains an entry without a key");
      }

      auto const key = keyView(child);

      if (std::ranges::contains(seenKeys, key))
      {
        return makeError(Error::Code::FormatRejected, fieldContext(context, key) + " appears more than once");
      }

      seenKeys.push_back(key);

      if (unknownKeyPolicy == UnknownKeyPolicy::Reject && !std::ranges::contains(allowedKeys, key))
      {
        return makeError(Error::Code::FormatRejected, fieldContext(context, key) + " is not supported");
      }
    }

    return {};
  }

  Result<ryml::ConstNodeRef> requireChild(ryml::ConstNodeRef node, std::string_view key, std::string_view context)
  {
    auto const child = findChild(node, key);

    if (!child.readable())
    {
      return makeError(Error::Code::FormatRejected, fieldContext(context, key) + " is required");
    }

    return child;
  }

  ryml::NodeRef appendChild(ryml::NodeRef node, std::string_view key)
  {
    auto child = node.append_child();
    setKey(child, key);
    return child;
  }

  void writeScalar(ryml::NodeRef node, std::string_view value)
  {
    setValue(node, value);
    node.set_val_style(ryml::VAL_DQUO);
  }

  void writeScalar(ryml::NodeRef node, std::string const& value)
  {
    writeScalar(node, std::string_view{value});
  }

  void writeScalar(ryml::NodeRef node, bool value)
  {
    node << c4::fmt::boolalpha(value);
  }

  MapWriter::MapWriter(ryml::NodeRef node)
    : _node{node}
  {
    _node |= ryml::MAP;
  }

  Result<> MapWriter::finish() &&
  {
    return std::move(_result);
  }

  MapReader::MapReader(ryml::ConstNodeRef node,
                       std::span<std::string_view const> allowedKeys,
                       std::string_view context,
                       UnknownKeyPolicy unknownKeyPolicy)
    : _node{node}
    , _context{boundedErrorContext(context)}
    , _result{validateMapKeys(node, allowedKeys, _context, unknownKeyPolicy)}
  {
  }
} // namespace ao::yaml
