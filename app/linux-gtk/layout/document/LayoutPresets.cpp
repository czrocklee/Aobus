// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "layout/document/LayoutPresets.h"

#include <ao/Exception.h>
#include <ao/ExceptionFormat.h>
#include <ao/rt/Log.h>
#include <ao/uimodel/layout/document/LayoutDocument.h>
#include <ao/uimodel/layout/document/LayoutYaml.h>
#include <ao/yaml/RymlAdapter.h>

#include <giomm/resource.h>
#include <glib.h>
#include <glibmm/error.h>

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

        auto docRes = uimodel::LayoutDocumentYamlSchema{}.deserialize(tree.rootref(), uimodel::LayoutDocument{});

        if (!docRes)
        {
          throwException<Exception>("Failed to deserialize built-in layout from {}: {}", path, docRes.error().message);
        }

        return std::move(*docRes);
      }
      catch (Glib::Error const& e)
      {
        APP_LOG_CRITICAL("LayoutPresets: GResource error loading {}: {}", path, e.what());
        throw;
      }
    }
  } // namespace

  LayoutPresetId presetIdFromString(std::string_view const presetId)
  {
    if (presetId == "modern")
    {
      return LayoutPresetId::Modern;
    }

    return LayoutPresetId::Classic;
  }

  std::string_view presetIdToString(LayoutPresetId const presetId) noexcept
  {
    switch (presetId)
    {
      case LayoutPresetId::Classic: return "classic";
      case LayoutPresetId::Modern: return "modern";
    }

    return "classic";
  }

  uimodel::LayoutDocument makeDefaultLayout()
  {
    return makeBuiltInLayout(LayoutPresetId::Classic);
  }

  uimodel::LayoutDocument makeBuiltInLayout(LayoutPresetId presetId)
  {
    switch (presetId)
    {
      case LayoutPresetId::Classic: return loadBuiltInLayout("/org/aobus/layout/default_layout.yaml");
      case LayoutPresetId::Modern:
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
} // namespace ao::gtk::layout
