// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/layout/document/LayoutPreparation.h>

#include <ao/Error.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::uimodel
{
  namespace
  {
    constexpr std::size_t kMaximumTemplateDiagnosticBytes = 160;

    class TreeBudgetMeter final
    {
    public:
      TreeBudgetMeter(LayoutTreeLimits const& limits, std::string_view stage)
        : _limits{limits}, _stage{stage}
      {
      }

      Result<> consumeDepth(std::size_t depth) const
      {
        if (depth > _limits.maxDepth)
        {
          return makeError(Error::Code::ValueTooLarge,
                           std::format("Layout {} depth {} exceeds limit {}", _stage, depth, _limits.maxDepth));
        }

        return {};
      }

      Result<> consumeEntries(std::size_t count) { return consume(count, _entries, _limits.maxEntries, "entries"); }

      Result<> consumeValueBytes(std::size_t count)
      {
        return consume(count, _valueBytes, _limits.maxValueBytes, "value bytes");
      }

      Result<> consumeText(std::string_view text) { return consumeValueBytes(text.size()); }

      Result<> consumeValue(LayoutValue const& value)
      {
        if (auto const* text = value.getIf<std::string>(); text != nullptr)
        {
          return consumeText(*text);
        }

        if (auto const* values = value.getIf<std::vector<std::string>>(); values != nullptr)
        {
          if (auto res = consumeEntries(values->size()); !res)
          {
            return res;
          }

          for (auto const& item : *values)
          {
            if (auto res = consumeText(item); !res)
            {
              return res;
            }
          }
        }

        return {};
      }

      Result<> consumeValueMap(LayoutValueMap const& values)
      {
        if (auto res = consumeEntries(values.size()); !res)
        {
          return res;
        }

        for (auto const& [key, value] : values)
        {
          if (auto res = consumeText(key); !res)
          {
            return res;
          }

          if (auto res = consumeValue(value); !res)
          {
            return res;
          }
        }

        return {};
      }

      Result<> consumeNode(LayoutNode const& node)
      {
        if (node.type.empty())
        {
          return makeError(Error::Code::FormatRejected, "Layout node type must not be empty");
        }

        if (auto res = consumeEntries(1); !res)
        {
          return res;
        }

        if (auto res = consumeText(node.id); !res)
        {
          return res;
        }

        if (auto res = consumeText(node.type); !res)
        {
          return res;
        }

        if (auto res = consumeValueMap(node.props); !res)
        {
          return res;
        }

        return consumeValueMap(node.layout);
      }

    private:
      Result<> consume(std::size_t count, std::size_t& consumed, std::size_t limit, std::string_view dimension)
      {
        if (consumed > limit || count > limit - consumed)
        {
          return makeError(
            Error::Code::ValueTooLarge, std::format("Layout {} {} exceed limit {}", _stage, dimension, limit));
        }

        consumed += count;
        return {};
      }

      LayoutTreeLimits const& _limits;
      std::string_view _stage;
      std::size_t _entries = 0;
      std::size_t _valueBytes = 0;
    };

    Result<> measureAuthoredNode(LayoutNode const& node, TreeBudgetMeter& meter, std::size_t depth)
    {
      if (auto res = meter.consumeDepth(depth); !res)
      {
        return res;
      }

      if (auto res = meter.consumeNode(node); !res)
      {
        return res;
      }

      for (auto const& child : node.children)
      {
        if (auto res = measureAuthoredNode(child, meter, depth + 1); !res)
        {
          return res;
        }
      }

      if (node.optTooltip && node.optTooltip->nodePtr)
      {
        return measureAuthoredNode(*node.optTooltip->nodePtr, meter, depth + 1);
      }

      return {};
    }

    Result<> measureAuthoredDocument(LayoutDocument const& document, TreeBudgetMeter& meter)
    {
      if (auto res = measureAuthoredNode(document.root, meter, 1); !res)
      {
        return res;
      }

      if (auto res = meter.consumeEntries(document.templates.size()); !res)
      {
        return res;
      }

      for (auto const& [templateId, root] : document.templates)
      {
        if (auto res = meter.consumeText(templateId); !res)
        {
          return res;
        }

        if (auto res = measureAuthoredNode(root, meter, 1); !res)
        {
          return res;
        }
      }

      return {};
    }

    std::string boundedTemplateDiagnostic(std::string text)
    {
      if (text.size() <= kMaximumTemplateDiagnosticBytes)
      {
        return text;
      }

      constexpr auto kSuffix = std::string_view{"..."};
      auto result = std::string{};
      result.reserve(kMaximumTemplateDiagnosticBytes);
      result.append(text.data(), kMaximumTemplateDiagnosticBytes - kSuffix.size());
      result.append(kSuffix);
      return result;
    }

    Result<LayoutNode> makeTemplateDiagnostic(std::string message, TreeBudgetMeter& meter, std::string_view id = {})
    {
      auto node = LayoutNode{.id = std::string{id}, .type = boundedTemplateDiagnostic(std::move(message))};

      if (auto res = meter.consumeNode(node); !res)
      {
        return std::unexpected{res.error()};
      }

      return node;
    }

    Result<> consumeOverride(LayoutValueMap const& values, TreeBudgetMeter& meter)
    {
      return meter.consumeValueMap(values);
    }

    // Template and concrete nodes share one recursive traversal so every produced value uses the same budget meter.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    Result<LayoutNode> expandNode(LayoutNode const& node,
                                  std::map<std::string, LayoutNode, std::less<>> const& templates,
                                  std::vector<std::string_view>& visited,
                                  TreeBudgetMeter& meter,
                                  std::size_t depth)
    {
      if (auto res = meter.consumeDepth(depth); !res)
      {
        return std::unexpected{res.error()};
      }

      if (node.type == "template")
      {
        auto const templateIdIt = node.props.find("templateId");
        auto const* templateId = templateIdIt == node.props.end() ? nullptr : templateIdIt->second.getIf<std::string>();

        if (templateId == nullptr || templateId->empty())
        {
          return makeTemplateDiagnostic("[TemplateError] Missing templateId", meter, node.id);
        }

        if (std::ranges::contains(visited, std::string_view{*templateId}))
        {
          auto chain = std::string{"[TemplateError] Recursive template loop: "};

          for (auto const visitedId : visited)
          {
            chain += visitedId;
            chain += " -> ";
          }

          chain += *templateId;
          return makeTemplateDiagnostic(std::move(chain), meter);
        }

        auto const it = templates.find(*templateId);

        if (it == templates.end())
        {
          return makeTemplateDiagnostic("[TemplateError] Unknown template: " + *templateId, meter);
        }

        visited.push_back(*templateId);
        auto expandedRes = expandNode(it->second, templates, visited, meter, depth + 1);
        visited.pop_back();

        if (!expandedRes)
        {
          return std::unexpected{expandedRes.error()};
        }

        if (!node.id.empty())
        {
          if (auto res = meter.consumeText(node.id); !res)
          {
            return std::unexpected{res.error()};
          }

          expandedRes->id = node.id;
        }

        if (auto res = consumeOverride(node.layout, meter); !res)
        {
          return std::unexpected{res.error()};
        }

        for (auto const& [key, value] : node.layout)
        {
          expandedRes->layout[key] = value;
        }

        for (auto const& [key, value] : node.props)
        {
          if (key == "templateId")
          {
            continue;
          }

          if (auto res = meter.consumeEntries(1); !res)
          {
            return std::unexpected{res.error()};
          }

          if (auto res = meter.consumeText(key); !res)
          {
            return std::unexpected{res.error()};
          }

          if (auto res = meter.consumeValue(value); !res)
          {
            return std::unexpected{res.error()};
          }

          expandedRes->props[key] = value;
        }

        for (auto const& child : node.children)
        {
          auto expandedChildRes = expandNode(child, templates, visited, meter, depth + 1);

          if (!expandedChildRes)
          {
            return std::unexpected{expandedChildRes.error()};
          }

          expandedRes->children.push_back(std::move(*expandedChildRes));
        }

        if (node.optTooltip && node.optTooltip->nodePtr)
        {
          auto expandedTooltipRes = expandNode(*node.optTooltip->nodePtr, templates, visited, meter, depth + 1);

          if (!expandedTooltipRes)
          {
            return std::unexpected{expandedTooltipRes.error()};
          }

          expandedRes->optTooltip = BoxedLayoutNode{std::move(*expandedTooltipRes)};
        }

        return expandedRes;
      }

      if (auto res = meter.consumeNode(node); !res)
      {
        return std::unexpected{res.error()};
      }

      auto result = LayoutNode{.id = node.id, .type = node.type, .props = node.props, .layout = node.layout};
      result.children.reserve(node.children.size());

      for (auto const& child : node.children)
      {
        auto expandedChildRes = expandNode(child, templates, visited, meter, depth + 1);

        if (!expandedChildRes)
        {
          return std::unexpected{expandedChildRes.error()};
        }

        result.children.push_back(std::move(*expandedChildRes));
      }

      if (node.optTooltip && node.optTooltip->nodePtr)
      {
        auto expandedTooltipRes = expandNode(*node.optTooltip->nodePtr, templates, visited, meter, depth + 1);

        if (!expandedTooltipRes)
        {
          return std::unexpected{expandedTooltipRes.error()};
        }

        result.optTooltip = BoxedLayoutNode{std::move(*expandedTooltipRes)};
      }

      return result;
    }
  } // namespace

  Result<PreparedLayout> prepareLayout(LayoutDocument const& document, LayoutDocumentLimits const& limits)
  {
    if (document.version != kLayoutDocumentVersion)
    {
      return makeError(
        Error::Code::NotSupported, std::format("Unsupported layout document version {}", document.version));
    }

    auto authoredMeter = TreeBudgetMeter{limits.authored, "authored"};

    if (auto res = measureAuthoredDocument(document, authoredMeter); !res)
    {
      return std::unexpected{res.error()};
    }

    auto effectiveMeter = TreeBudgetMeter{limits.effective, "effective"};
    auto visited = std::vector<std::string_view>{};
    auto effectiveRootRes = expandNode(document.root, document.templates, visited, effectiveMeter, 1);

    if (!effectiveRootRes)
    {
      return std::unexpected{effectiveRootRes.error()};
    }

    return PreparedLayout{std::move(*effectiveRootRes)};
  }
} // namespace ao::uimodel
