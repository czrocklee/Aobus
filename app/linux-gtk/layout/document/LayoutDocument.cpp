// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "layout/document/LayoutDocument.h"
#include "layout/document/LayoutYaml.h"
#include "layout/LayoutConstants.h"

#include <ao/utility/Log.h>
#include <stdexcept>

namespace YAML
{
  Node convert<ao::gtk::layout::LayoutValue>::encode(ao::gtk::layout::LayoutValue const& rhs)
  {
    return std::visit(
      [](auto const& nodeValue) -> Node
      {
        using T = std::decay_t<decltype(nodeValue)>;

        if constexpr (std::is_same_v<T, std::monostate>)
        {
          return Node(NodeType::Null);
        }
        else
        {
          return Node(nodeValue);
        }
      },
      rhs.data);
  }

  bool convert<ao::gtk::layout::LayoutValue>::decode(Node const& node, ao::gtk::layout::LayoutValue& rhs)
  {
    if (!node.IsDefined() || node.IsNull())
    {
      rhs.data = std::monostate{};
      return true;
    }

    if (node.IsScalar())
    {
      auto const& scalar = node.Scalar();

      if (scalar == "true")
      {
        rhs.data = true;
        return true;
      }

      if (scalar == "false")
      {
        rhs.data = false;
        return true;
      }

      try
      {
        std::size_t pos = 0;
        long long const intValue = std::stoll(scalar, &pos);

        if (pos == scalar.size())
        {
          rhs.data = static_cast<std::int64_t>(intValue);
          return true;
        }
      }
      catch (std::exception const& /*ex*/)
      {
        APP_LOG_TRACE("LayoutDocument: Failed to parse scalar '{}' as integer, trying double", scalar);
      }

      try
      {
        std::size_t pos = 0;
        double const doubleValue = std::stod(scalar, &pos);

        if (pos == scalar.size())
        {
          rhs.data = doubleValue;
          return true;
        }
      }
      catch (std::exception const& /*ex*/)
      {
        APP_LOG_TRACE("LayoutDocument: Failed to parse scalar '{}' as numeric, keeping as string", scalar);
      }

      rhs.data = scalar;
      return true;
    }

    if (node.IsSequence())
    {
      auto sequence = std::vector<std::string>{};

      for (auto const& item : node)
      {
        if (item.IsScalar())
        {
          sequence.push_back(item.as<std::string>());
        }
      }

      rhs.data = std::move(sequence);
      return true;
    }

    return false;
  }

  Node convert<ao::gtk::layout::LayoutNode>::encode(ao::gtk::layout::LayoutNode const& rhs)
  {
    Node node;

    if (!rhs.id.empty())
    {
      node["id"] = rhs.id;
    }

    node["type"] = rhs.type;

    if (!rhs.props.empty())
    {
      node["props"] = rhs.props;
    }

    if (!rhs.layout.empty())
    {
      node["layout"] = rhs.layout;
    }

    if (!rhs.children.empty())
    {
      node["children"] = rhs.children;
    }

    return node;
  }

  bool convert<ao::gtk::layout::LayoutNode>::decode(Node const& node, ao::gtk::layout::LayoutNode& rhs)
  {
    if (!node.IsMap())
    {
      return false;
    }

    if (node["id"])
    {
      rhs.id = node["id"].as<std::string>();
    }

    if (node["type"])
    {
      rhs.type = node["type"].as<std::string>();
    }

    if (node["props"])
    {
      rhs.props = node["props"].as<std::map<std::string, ao::gtk::layout::LayoutValue, std::less<>>>();
    }

    if (node["layout"])
    {
      rhs.layout = node["layout"].as<std::map<std::string, ao::gtk::layout::LayoutValue, std::less<>>>();
    }

    if (node["children"])
    {
      rhs.children = node["children"].as<std::vector<ao::gtk::layout::LayoutNode>>();
    }

    return true;
  }

  Node convert<ao::gtk::layout::LayoutDocument>::encode(ao::gtk::layout::LayoutDocument const& rhs)
  {
    Node node;
    node["version"] = rhs.version;
    node["root"] = rhs.root;

    if (!rhs.templates.empty())
    {
      node["templates"] = rhs.templates;
    }

    return node;
  }

  bool convert<ao::gtk::layout::LayoutDocument>::decode(Node const& node, ao::gtk::layout::LayoutDocument& rhs)
  {
    if (!node.IsMap() || !node["version"] || !node["root"])
    {
      return false;
    }

    rhs.version = node["version"].as<int>();
    rhs.root = node["root"].as<ao::gtk::layout::LayoutNode>();

    if (node["templates"])
    {
      rhs.templates = node["templates"].as<std::map<std::string, ao::gtk::layout::LayoutNode, std::less<>>>();
    }

    return true;
  }
} // namespace YAML

namespace ao::gtk::layout
{
  LayoutDocument createDefaultLayout()
  {
    auto doc = LayoutDocument{.version = 1};

    doc.root = LayoutNode{
      .id = "app-root",
      .type = "box",
      .props = {{"orientation", LayoutValue{std::string{"vertical"}}}},
      .children = {
        LayoutNode{.type = "app.menuBar"},
        LayoutNode{.id = "playback-row",
                   .type = "box",
                   .props = {{"orientation", LayoutValue{std::string{"horizontal"}}},
                             {"spacing", LayoutValue{static_cast<std::int64_t>(::ao::gtk::Layout::kSpacingMedium)}}},

                   .children = {LayoutNode{.type = "playback.outputButton"},
                                LayoutNode{.type = "playback.playPauseButton"},
                                LayoutNode{.type = "playback.stopButton"},
                                LayoutNode{.type = "playback.seekSlider", .layout = {{"hexpand", LayoutValue{true}}}},
                                LayoutNode{.type = "playback.timeLabel"},
                                LayoutNode{.type = "playback.volumeControl"}}},
        LayoutNode{
          .id = "main-paned",
          .type = "split",
          .props = {{"orientation", LayoutValue{std::string{"horizontal"}}},
                    {"position", LayoutValue{static_cast<std::int64_t>(::ao::gtk::Layout::kDefaultSidebarWidth)}}},
          .layout = {{"vexpand", LayoutValue{true}}},
          .children =
            {LayoutNode{
               .id = "left-sidebar",
               .type = "box",
               .props = {{"orientation", LayoutValue{std::string{"vertical"}}}},
               .children = {LayoutNode{.type = "library.listTree", .layout = {{"vexpand", LayoutValue{true}}}},
                            LayoutNode{.type = "inspector.coverArt",
                                       .layout = {{"minHeight", LayoutValue{static_cast<std::int64_t>(::ao::gtk::Layout::kMinCoverArtHeight)}}}}}},
             LayoutNode{.id = "workspace-with-inspector", .type = "app.workspaceWithInspector"}}},
        LayoutNode{.type = "status.defaultBar"}}};

    doc.templates = getBuiltInTemplates();

    return doc;
  }

  std::map<std::string, LayoutNode, std::less<>> getBuiltInTemplates()
  {
    auto templates = std::map<std::string, LayoutNode, std::less<>>{};

    // playback.compactControls
    {
      auto node = LayoutNode{.type = "box"};
      node.props["orientation"] = LayoutValue{std::string{"horizontal"}};

      int const compactSpacing = 4;
      node.props["spacing"] = LayoutValue{static_cast<std::int64_t>(compactSpacing)};
      node.children.push_back(LayoutNode{.type = "playback.playPauseButton"});
      node.children.push_back(LayoutNode{.type = "playback.stopButton"});
      node.children.push_back(LayoutNode{.type = "playback.timeLabel"});
      templates["playback.compactControls"] = std::move(node);
    }

    // playback.defaultBar
    {
      auto node = LayoutNode{.type = "box"};
      node.props["orientation"] = LayoutValue{std::string{"horizontal"}};

      int const barSpacing = 6;
      node.props["spacing"] = LayoutValue{static_cast<std::int64_t>(barSpacing)};
      node.children.push_back(LayoutNode{.type = "playback.outputButton"});
      node.children.push_back(LayoutNode{.type = "playback.playPauseButton"});
      node.children.push_back(LayoutNode{.type = "playback.stopButton"});
      auto seek = LayoutNode{.type = "playback.seekSlider"};
      seek.layout["hexpand"] = LayoutValue{true};
      node.children.push_back(std::move(seek));
      node.children.push_back(LayoutNode{.type = "playback.timeLabel"});
      node.children.push_back(LayoutNode{.type = "playback.volumeControl"});
      templates["playback.defaultBar"] = std::move(node);
    }

    // library.defaultSidebar
    {
      auto node = LayoutNode{.type = "box"};
      node.props["orientation"] = LayoutValue{std::string{"vertical"}};
      auto tree = LayoutNode{.type = "library.listTree"};
      tree.layout["vexpand"] = LayoutValue{true};
      node.children.push_back(std::move(tree));
      auto cover = LayoutNode{.type = "inspector.coverArt"};
      int const coverMinHeight = 50;
      cover.layout["minHeight"] = LayoutValue{static_cast<std::int64_t>(coverMinHeight)};
      node.children.push_back(std::move(cover));
      templates["library.defaultSidebar"] = std::move(node);
    }

    // inspector.defaultPanel
    {
      templates["inspector.defaultPanel"] = LayoutNode{.type = "inspector.sidebar"};
    }

    // status.defaultBar
    {
      templates["status.defaultBar"] = LayoutNode{.type = "status.defaultBar"};
    }

    // tracks.defaultWorkspace
    {
      auto node = LayoutNode{.type = "tracks.table"};
      node.props["view"] = LayoutValue{std::string{"workspace.focused"}};
      node.layout["vexpand"] = LayoutValue{true};
      templates["tracks.defaultWorkspace"] = std::move(node);
    }

    // app.defaultLayout
    {
      templates["app.defaultLayout"] = LayoutNode{.type = "app.workspaceWithInspector"};
    }

    return templates;
  }
} // namespace ao::gtk::layout
