// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/document/GtkLayoutPresets.h"

#include <ao/Exception.h>
#include <ao/rt/Log.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutNode.h>
#include <ao/uimodel/layout/document/LayoutYaml.h>
#include <ao/yaml/RymlAdapter.h>

#include <giomm/resource.h>
#include <glib.h>
#include <glibmm/error.h>

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk::layout
{
  namespace
  {
    uimodel::LayoutDocument loadBuiltInLayout(std::string_view path)
    {
      try
      {
        auto const bytesPtr = Gio::Resource::lookup_data_global(std::string{path});
        gsize size = 0;
        auto const* const data = static_cast<char const*>(bytesPtr->get_data(size));

        auto yamlErrorState = yaml::ErrorCallbackState{std::string{path}};
        auto tree = ryml::Tree{yaml::callbacks(yamlErrorState)};
        yaml::parseInArena(tree, std::string_view{data, size}, yamlErrorState);

        auto doc = uimodel::LayoutDocumentYamlSchema{}.deserialize(tree.rootref(), uimodel::LayoutDocument{});

        if (!doc)
        {
          throwException<Exception>("Failed to deserialize built-in layout from {}: {}", path, doc.error().message);
        }

        return std::move(*doc);
      }
      catch (Glib::Error const& e)
      {
        APP_LOG_CRITICAL("GtkLayoutPresets: GResource error loading {}: {}", path, e.what());
        throw;
      }
    }
  } // namespace

  GtkLayoutPresetId presetIdFromString(std::string_view presetIdStr)
  {
    if (presetIdStr == "modern")
    {
      return GtkLayoutPresetId::Modern;
    }

    return GtkLayoutPresetId::Classic;
  }

  uimodel::LayoutDocument makeDefaultGtkLayout()
  {
    return makeBuiltInGtkLayout(GtkLayoutPresetId::Classic);
  }

  uimodel::LayoutDocument makeBuiltInGtkLayout(GtkLayoutPresetId presetId)
  {
    switch (presetId)
    {
      case GtkLayoutPresetId::Classic: return loadBuiltInLayout("/org/aobus/layout/default_layout.yaml");
      case GtkLayoutPresetId::Modern:
      {
        auto doc = loadBuiltInLayout("/org/aobus/layout/modern_layout.yaml");
        auto const defaultDoc = loadBuiltInLayout("/org/aobus/layout/default_layout.yaml");

        for (auto const& [name, templateNode] : defaultDoc.templates)
        {
          doc.templates.try_emplace(name, templateNode);
        }

        return doc;
      }
    }

    return loadBuiltInLayout("/org/aobus/layout/default_layout.yaml");
  }

  std::map<std::string, uimodel::LayoutNode, std::less<>> builtInGtkTemplates()
  {
    return makeDefaultGtkLayout().templates;
  }
} // namespace ao::gtk::layout
