// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "app/linux-gtk/layout/state/ILayoutComponentStateStore.h"
#include "app/linux-gtk/layout/state/LayoutComponentState.h"
#include <ao/uimodel/layout/LayoutDocument.h>

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace ao::gtk::layout::test
{
  class FakeLayoutComponentStateStore final : public ILayoutComponentStateStore
  {
  public:
    std::optional<LayoutComponentStateDocument> load(std::string_view presetId) const override
    {
      if (_document.preset == presetId)
      {
        return _document;
      }

      return std::nullopt;
    }

    void save(LayoutComponentStateDocument const& doc, std::string_view presetId) override
    {
      _document = doc;
      _document.preset = presetId;
      ++_saveCount;
    }

    bool prune(std::string_view /*presetId*/, uimodel::layout::LayoutDocument const& /*effectiveDoc*/) override
    {
      return false;
    }

    bool removePreset(std::string_view presetId) override
    {
      if (_document.preset == presetId)
      {
        _document.components.clear();
        return true;
      }

      return false;
    }

    LayoutComponentStateDocument const& document() const noexcept { return _document; }
    void setDocument(LayoutComponentStateDocument doc) { _document = std::move(doc); }

    std::int32_t saveCount() const noexcept { return _saveCount; }

  private:
    LayoutComponentStateDocument _document{};
    std::int32_t _saveCount = 0;
  };
} // namespace ao::gtk::layout::test
