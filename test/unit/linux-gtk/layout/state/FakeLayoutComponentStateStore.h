// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/ILayoutComponentStateStore.h>
#include <ao/uimodel/layout/LayoutComponentState.h>

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace ao::uimodel::layout
{
  struct LayoutDocument;
}

namespace ao::gtk::layout::test
{
  class FakeLayoutComponentStateStore final : public uimodel::layout::ILayoutComponentStateStore
  {
  public:
    std::optional<uimodel::layout::LayoutComponentStateDocument> load(std::string_view presetId) const override
    {
      if (_document.preset == presetId)
      {
        return _document;
      }

      return std::nullopt;
    }

    void save(std::string_view presetId, uimodel::layout::LayoutComponentStateDocument const& doc) override
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

    uimodel::layout::LayoutComponentStateDocument const& document() const noexcept { return _document; }
    void setDocument(uimodel::layout::LayoutComponentStateDocument doc) { _document = std::move(doc); }

    std::int32_t saveCount() const noexcept { return _saveCount; }

  private:
    uimodel::layout::LayoutComponentStateDocument _document{};
    std::int32_t _saveCount = 0;
  };
} // namespace ao::gtk::layout::test
