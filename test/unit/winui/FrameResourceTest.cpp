// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>
#include <ao/winui/layout/ElementKind.h>
#include <ao/winui/layout/LayoutCatalog.h>
#include <ao/winui/layout/ShellDocument.h>
#include <ao/winui/layout/StyleLookup.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ao::winui::test
{
  namespace
  {
    /**
     * @brief A resource the window frame declares directly in the window scope.
     *
     * `styleKey` resolves only against `RootGrid.Resources`, so what matters is
     * not that a key appears somewhere in the frame but that it appears as a
     * direct entry of that dictionary: a key nested inside another keyed
     * `ResourceDictionary` is out of scope for a document.
     */
    struct FrameResource final
    {
      std::string type;
      std::string key;
      std::string targetType;
      /// `Setter` property to value, for the resources that are styles.
      std::map<std::string, std::string> setters;
    };

    std::string readFile(std::filesystem::path const& path)
    {
      auto stream = std::ifstream{path, std::ios::binary};
      REQUIRE(stream.is_open());
      return std::string{std::istreambuf_iterator{stream}, std::istreambuf_iterator<char>{}};
    }

    std::string readShippedDocument(ShellPreset const preset)
    {
      return readFile(std::filesystem::path{AOBUS_WINDOWS_LAYOUT_DIR} / shellPresetResource(preset));
    }

    /// The value of attribute @p name in @p tag, or nullopt when it carries none.
    std::optional<std::string> attribute(std::string_view const tag, std::string_view const name)
    {
      auto const marker = std::string{name} + "=\"";
      auto const start = tag.find(marker);

      if (start == std::string_view::npos)
      {
        return std::nullopt;
      }

      auto const from = start + marker.size();
      auto const end = tag.find('"', from);

      if (end == std::string_view::npos)
      {
        return std::nullopt;
      }

      return std::string{tag.substr(from, end - from)};
    }

    /// The element name a tag opens or closes, without its namespace prefix.
    std::string tagName(std::string_view const tag)
    {
      auto body = tag.substr(1, tag.size() - 2);

      if (!body.empty() && body.front() == '/')
      {
        body.remove_prefix(1);
      }

      auto const end = body.find_first_of(" \t\r\n/");
      return std::string{end == std::string_view::npos ? body : body.substr(0, end)};
    }

    /// Whether @p tag closes the element it names rather than opening one.
    bool isClosingTag(std::string_view const tag)
    {
      return tag.size() > 1 && tag[1] == '/';
    }

    bool isSelfClosingTag(std::string_view const tag)
    {
      return tag.size() > 2 && tag[tag.size() - 2] == '/';
    }

    /**
     * @brief Every tag in @p xaml, in document order.
     *
     * Attribute values are skipped over rather than scanned, so a `>` inside a
     * binding or a margin cannot end a tag early, and comments are dropped.
     */
    std::vector<std::string> xamlTags(std::string_view const xaml)
    {
      auto tags = std::vector<std::string>{};
      std::size_t position = 0;

      while (position < xaml.size())
      {
        auto const open = xaml.find('<', position);

        if (open == std::string_view::npos)
        {
          break;
        }

        if (xaml.compare(open, 4, "<!--") == 0)
        {
          auto const closed = xaml.find("-->", open);
          position = closed == std::string_view::npos ? xaml.size() : closed + 3;
          continue;
        }

        auto scan = open + 1;
        char quote = 0;

        while (scan < xaml.size())
        {
          if (auto const character = xaml[scan]; quote != 0)
          {
            quote = character == quote ? char{0} : quote;
          }
          else if (character == '"' || character == '\'')
          {
            quote = character;
          }
          else if (character == '>')
          {
            break;
          }

          ++scan;
        }

        if (scan >= xaml.size())
        {
          break;
        }

        tags.emplace_back(xaml.substr(open, scan - open + 1));
        position = scan + 1;
      }

      return tags;
    }

    /**
     * @brief The direct entries of the window's own `RootGrid.Resources`.
     *
     * The first `Grid.Resources` in the frame belongs to `RootGrid`, which is
     * the outermost element in the window, and the walk stops at its close so
     * that dictionaries declared inside templates stay out of the result.
     */
    std::vector<FrameResource> windowScopeResources()
    {
      auto const tags = xamlTags(readFile(std::filesystem::path{AOBUS_WINDOWS_FRAME_XAML}));
      auto const opening = std::ranges::find_if(
        tags, [](std::string const& tag) { return !isClosingTag(tag) && tagName(tag) == "Grid.Resources"; });

      REQUIRE(opening != tags.end());

      auto resources = std::vector<FrameResource>{};
      std::int32_t depth = 0;

      for (auto tag = std::next(opening); tag != tags.end(); ++tag)
      {
        if (isClosingTag(*tag))
        {
          if (depth == 0)
          {
            break;
          }

          --depth;
          continue;
        }

        if (depth == 0)
        {
          if (auto optKey = attribute(*tag, "x:Key"); optKey)
          {
            resources.push_back(FrameResource{.type = tagName(*tag),
                                              .key = *std::move(optKey),
                                              .targetType = attribute(*tag, "TargetType").value_or(std::string{}),
                                              .setters = {}});
          }
        }
        else if (depth == 1 && tagName(*tag) == "Setter" && !resources.empty())
        {
          auto optProperty = attribute(*tag, "Property");

          if (auto optValue = attribute(*tag, "Value"); optProperty && optValue)
          {
            resources.back().setters.insert_or_assign(*std::move(optProperty), *std::move(optValue));
          }
        }

        depth += isSelfClosingTag(*tag) ? 0 : 1;
      }

      return resources;
    }

    FrameResource const* findResource(std::vector<FrameResource> const& resources, std::string_view const key)
    {
      auto const found = std::ranges::find(resources, key, &FrameResource::key);
      return found == resources.end() ? nullptr : &*found;
    }

    /// The XAML type name a `TargetType` names, reduced to its unqualified spelling.
    std::string_view unqualified(std::string_view const name)
    {
      auto const separator = name.rfind('.');
      return separator == std::string_view::npos ? name : name.substr(separator + 1);
    }

    void collectNodes(uimodel::LayoutNode const& node, std::vector<uimodel::LayoutNode const*>& nodes)
    {
      nodes.push_back(&node);

      for (auto const& child : node.children)
      {
        collectNodes(child, nodes);
      }
    }

    std::vector<uimodel::LayoutNode const*> presetNodes(uimodel::PreparedLayout const& layout)
    {
      auto nodes = std::vector<uimodel::LayoutNode const*>{};
      collectNodes(layout.effectiveRoot(), nodes);
      return nodes;
    }
  } // namespace

  TEST_CASE("FrameResource - the window frame declares every style the shipped presets name", "[winui][unit][layout]")
  {
    // A preset that names a key the frame does not declare builds an unstyled
    // shell, and one whose element the style cannot target fails the build
    // outright. Both are decided by two files that are edited independently, so
    // the pairing is checked here rather than discovered on Windows.
    auto const resources = windowScopeResources();
    std::size_t styled = 0;

    for (auto const preset : {ShellPreset::Modern, ShellPreset::Classic})
    {
      auto const resource = shellPresetResource(preset);
      INFO("preset " << resource);

      auto const prepared = prepareShellPresetDocument(readShippedDocument(preset), resource);
      REQUIRE(prepared);

      for (auto const* const node : presetNodes(*prepared))
      {
        auto const optKind = componentElementKind(*node);
        REQUIRE(optKind);

        auto const optPlan = planStyleLookup(*node, *optKind);

        if (!optPlan)
        {
          continue;
        }

        INFO("node " << node->id << " names " << optPlan->key);
        auto const* const style = findResource(resources, optPlan->key);

        REQUIRE(style != nullptr);
        CHECK(style->type == "Style");

        auto const optTarget = elementKindFromString(unqualified(style->targetType));
        REQUIRE(optTarget);
        CHECK(resolveStyle(optPlan, StyleScope::RootGridResources, optTarget) == StyleResolution::Applied);
        ++styled;
      }
    }

    // Both presets style most of their structure, so a walk that found almost
    // nothing means the documents or the frame stopped being readable here.
    CHECK(styled >= 20);
  }

  TEST_CASE("FrameResource - the window frame declares the resources the Windows components resolve by name",
            "[winui][unit][layout]")
  {
    // These are not reachable through `styleKey`: a template, a container
    // style, and the per-presentation slider chrome are handed to the component
    // that needs them. The component looks each one up by name, so a rename in
    // the frame leaves a silently unstyled control.
    auto const resources = windowScopeResources();

    for (auto const* const key : {
           "TrackHeaderCellTemplate",     // TrackTable, column headers
           "TrackRowTemplate",            // TrackTable, rows
           "TrackListItemStyle",          // TrackTable, row containers
           "InspectorSectionHeaderStyle", // TrackDetail, section headers
           "NavigationTreeNodeTemplate",  // NavigationPane, tree presentation
           "InspectorOverlayFillBrush"    // ShellRegistry, revealed inspector overlay
         })
    {
      INFO("resource " << key);
      CHECK(findResource(resources, key) != nullptr);
    }

    for (auto const* const key : {"ModernSeekOverlayResources", "ClassicSeekInlineResources"})
    {
      INFO("resource " << key);
      auto const* const dictionary = findResource(resources, key);

      REQUIRE(dictionary != nullptr);
      CHECK(dictionary->type == "ResourceDictionary");
    }
  }

  TEST_CASE("FrameResource - the window frame declares the Classic chrome rounding a retro theme replaces",
            "[winui][unit][layout]")
  {
    // The shell answers `classic.chrome` by replacing this value and rebuilding
    // rather than by reaching for the bars a document built. A `ThemeResource`
    // that resolves to nothing throws when the style is applied, so the key has
    // to be declared here even though the shell always overwrites it.
    auto const resources = windowScopeResources();
    auto const* const rounding = findResource(resources, "ClassicChromeCornerRadius");

    REQUIRE(rounding != nullptr);
    CHECK(rounding->type == "CornerRadius");

    for (auto const* const key : {"ClassicToolbarStyle", "ClassicStatusBarStyle"})
    {
      INFO("style " << key);
      auto const* const style = findResource(resources, key);

      REQUIRE(style != nullptr);
      CHECK(style->setters.contains("CornerRadius"));
      CHECK(style->setters.at("CornerRadius") == "{ThemeResource ClassicChromeCornerRadius}");
    }
  }

  TEST_CASE("FrameResource - the window frame keeps the seek slider chrome out of the window scope",
            "[winui][unit][layout]")
  {
    // The slider chrome keys are WinUI's own. Declared in the window they would
    // re-chrome every slider in the shell, so they live inside a keyed
    // dictionary that the seek component merges into the one slider that asked
    // for it, which also puts them out of reach of any document.
    auto const resources = windowScopeResources();

    for (auto const* const key : {"ModernSeekThumbTemplate", "SliderTrackThemeHeight", "SliderHorizontalThumbWidth"})
    {
      INFO("resource " << key);
      CHECK(findResource(resources, key) == nullptr);
    }

    CHECK(findResource(resources, "ChromeLessButtonStyle") != nullptr);
  }
} // namespace ao::winui::test
