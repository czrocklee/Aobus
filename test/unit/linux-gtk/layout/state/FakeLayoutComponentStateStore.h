// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/component/ILayoutComponentStateStore.h>
#include <ao/uimodel/layout/component/LayoutComponentState.h>

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace ao::uimodel
{
  struct LayoutDocument;
}

namespace ao::gtk::layout::test
{
  class FakeLayoutComponentStateStore final : public uimodel::ILayoutComponentStateStore
  {
  public:
    std::optional<uimodel::LayoutComponentStateDocument> load(std::string_view presetId) const override
    {
      if (_document.preset == presetId)
      {
        return _document;
      }

      return std::nullopt;
    }

    void save(std::string_view presetId, uimodel::LayoutComponentStateDocument const& doc) override
    {
      _document = doc;
      _document.preset = presetId;
      ++_saveCount;
    }

    bool prune(std::string_view /*presetId*/, uimodel::LayoutDocument const& /*effectiveDoc*/) override
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

    uimodel::LayoutComponentStateDocument const& document() const noexcept { return _document; }
    void setDocument(uimodel::LayoutComponentStateDocument doc) { _document = std::move(doc); }

    std::int32_t saveCount() const noexcept { return _saveCount; }

  private:
    uimodel::LayoutComponentStateDocument _document{};
    std::int32_t _saveCount = 0;
  };
} // namespace ao::gtk::layout::test
