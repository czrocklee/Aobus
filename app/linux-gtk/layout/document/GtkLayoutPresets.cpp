// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/document/GtkLayoutPresets.h"

#include <ao/Exception.h>
#include <ao/uimodel/layout/LayoutDocument.h>
#include <ao/uimodel/layout/LayoutNode.h>
#include <ao/uimodel/layout/LayoutYaml.h>
#include <ao/utility/Log.h>
#include <ao/yaml/Utils.h>

#include <giomm/resource.h>
#include <glib.h>
#include <glibmm/error.h>

#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace ao::gtk::layout
{
  namespace
  {
    uimodel::layout::LayoutDocument loadBuiltInLayout(std::string_view path)
    {
      try
      {
        auto const bytesPtr = Gio::Resource::lookup_data_global(std::string{path});
        gsize size = 0;
        auto const* const data = static_cast<char const*>(bytesPtr->get_data(size));

        auto tree = ryml::Tree{yaml::callbacks(std::string{path}.c_str())};
        ryml::parse_in_arena(yaml::toCsubstr(std::string_view{data, size}), &tree);

        auto doc = uimodel::layout::LayoutDocument{};

        if (!yaml::read(tree.rootref(), doc))
        {
          throwException<Exception>("Failed to decode built-in layout from {}", path);
        }

        return doc;
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

  uimodel::layout::LayoutDocument createDefaultGtkLayout()
  {
    return createBuiltInGtkLayout(GtkLayoutPresetId::Classic);
  }

  uimodel::layout::LayoutDocument createBuiltInGtkLayout(GtkLayoutPresetId presetId)
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

  std::map<std::string, uimodel::layout::LayoutNode, std::less<>> getBuiltInGtkTemplates()
  {
    return createDefaultGtkLayout().templates;
  }
} // namespace ao::gtk::layout
