// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/Error.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <new>
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
          if (auto result = consumeEntries(values->size()); !result)
          {
            return result;
          }

          for (auto const& item : *values)
          {
            if (auto result = consumeText(item); !result)
            {
              return result;
            }
          }
        }

        return {};
      }

      Result<> consumeValueMap(LayoutValueMap const& values)
      {
        if (auto result = consumeEntries(values.size()); !result)
        {
          return result;
        }

        for (auto const& [key, value] : values)
        {
          if (auto result = consumeText(key); !result)
          {
            return result;
          }

          if (auto result = consumeValue(value); !result)
          {
            return result;
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

        if (auto result = consumeEntries(1); !result)
        {
          return result;
        }

        if (auto result = consumeText(node.id); !result)
        {
          return result;
        }

        if (auto result = consumeText(node.type); !result)
        {
          return result;
        }

        if (auto result = consumeValueMap(node.props); !result)
        {
          return result;
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
      if (auto result = meter.consumeDepth(depth); !result)
      {
        return result;
      }

      if (auto result = meter.consumeNode(node); !result)
      {
        return result;
      }

      for (auto const& child : node.children)
      {
        if (auto result = measureAuthoredNode(child, meter, depth + 1); !result)
        {
          return result;
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
      if (auto result = measureAuthoredNode(document.root, meter, 1); !result)
      {
        return result;
      }

      if (auto result = meter.consumeEntries(document.templates.size()); !result)
      {
        return result;
      }

      for (auto const& [templateId, root] : document.templates)
      {
        if (auto result = meter.consumeText(templateId); !result)
        {
          return result;
        }

        if (auto result = measureAuthoredNode(root, meter, 1); !result)
        {
          return result;
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
      text.resize(kMaximumTemplateDiagnosticBytes - kSuffix.size());
      text += kSuffix;
      return text;
    }

    Result<LayoutNode> makeTemplateDiagnostic(std::string message, TreeBudgetMeter& meter, std::string_view id = {})
    {
      auto node = LayoutNode{.id = std::string{id}, .type = boundedTemplateDiagnostic(std::move(message))};

      if (auto result = meter.consumeNode(node); !result)
      {
        return std::unexpected{result.error()};
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
      if (auto result = meter.consumeDepth(depth); !result)
      {
        return std::unexpected{result.error()};
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
        auto expanded = expandNode(it->second, templates, visited, meter, depth + 1);
        visited.pop_back();

        if (!expanded)
        {
          return std::unexpected{expanded.error()};
        }

        if (!node.id.empty())
        {
          if (auto result = meter.consumeText(node.id); !result)
          {
            return std::unexpected{result.error()};
          }

          expanded->id = node.id;
        }

        if (auto result = consumeOverride(node.layout, meter); !result)
        {
          return std::unexpected{result.error()};
        }

        for (auto const& [key, value] : node.layout)
        {
          expanded->layout[key] = value;
        }

        for (auto const& [key, value] : node.props)
        {
          if (key == "templateId")
          {
            continue;
          }

          if (auto result = meter.consumeEntries(1); !result)
          {
            return std::unexpected{result.error()};
          }

          if (auto result = meter.consumeText(key); !result)
          {
            return std::unexpected{result.error()};
          }

          if (auto result = meter.consumeValue(value); !result)
          {
            return std::unexpected{result.error()};
          }

          expanded->props[key] = value;
        }

        for (auto const& child : node.children)
        {
          auto expandedChild = expandNode(child, templates, visited, meter, depth + 1);

          if (!expandedChild)
          {
            return std::unexpected{expandedChild.error()};
          }

          expanded->children.push_back(std::move(*expandedChild));
        }

        if (node.optTooltip && node.optTooltip->nodePtr)
        {
          auto expandedTooltip = expandNode(*node.optTooltip->nodePtr, templates, visited, meter, depth + 1);

          if (!expandedTooltip)
          {
            return std::unexpected{expandedTooltip.error()};
          }

          expanded->optTooltip = BoxedLayoutNode{std::move(*expandedTooltip)};
        }

        return expanded;
      }

      if (auto result = meter.consumeNode(node); !result)
      {
        return std::unexpected{result.error()};
      }

      auto result = LayoutNode{.id = node.id, .type = node.type, .props = node.props, .layout = node.layout};
      result.children.reserve(node.children.size());

      for (auto const& child : node.children)
      {
        auto expandedChild = expandNode(child, templates, visited, meter, depth + 1);

        if (!expandedChild)
        {
          return std::unexpected{expandedChild.error()};
        }

        result.children.push_back(std::move(*expandedChild));
      }

      if (node.optTooltip && node.optTooltip->nodePtr)
      {
        auto expandedTooltip = expandNode(*node.optTooltip->nodePtr, templates, visited, meter, depth + 1);

        if (!expandedTooltip)
        {
          return std::unexpected{expandedTooltip.error()};
        }

        result.optTooltip = BoxedLayoutNode{std::move(*expandedTooltip)};
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

    try
    {
      auto authoredMeter = TreeBudgetMeter{limits.authored, "authored"};

      if (auto result = measureAuthoredDocument(document, authoredMeter); !result)
      {
        return std::unexpected{result.error()};
      }

      auto effectiveMeter = TreeBudgetMeter{limits.effective, "effective"};
      auto visited = std::vector<std::string_view>{};
      auto effectiveRoot = expandNode(document.root, document.templates, visited, effectiveMeter, 1);

      if (!effectiveRoot)
      {
        return std::unexpected{effectiveRoot.error()};
      }

      return PreparedLayout{std::move(*effectiveRoot)};
    }
    catch (std::bad_alloc const&)
    {
      return makeError(Error::Code::ResourceExhausted, "Insufficient memory to prepare layout document");
    }
  }
} // namespace ao::uimodel
