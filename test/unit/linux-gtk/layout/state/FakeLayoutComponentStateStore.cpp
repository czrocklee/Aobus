// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "FakeLayoutComponentStateStore.h"

#include <ao/uimodel/layout/component/LayoutComponentState.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace ao::gtk::layout::test
{
  struct FakeLayoutComponentStateStore::State final
  {
    uimodel::LayoutComponentStateDocument document{};
    std::int32_t saveCount = 0;
  };

  FakeLayoutComponentStateStore::FakeLayoutComponentStateStore()
    : _statePtr{std::make_unique<State>()}
  {
  }

  FakeLayoutComponentStateStore::~FakeLayoutComponentStateStore() = default;

  std::optional<uimodel::LayoutComponentStateDocument> FakeLayoutComponentStateStore::load(
    std::string_view const presetId) const
  {
    if (_statePtr->document.preset == presetId)
    {
      return _statePtr->document;
    }

    return std::nullopt;
  }

  void FakeLayoutComponentStateStore::save(std::string_view const presetId,
                                           uimodel::LayoutComponentStateDocument const& doc)
  {
    _statePtr->document = doc;
    _statePtr->document.preset = presetId;
    ++_statePtr->saveCount;
  }

  bool FakeLayoutComponentStateStore::prune(std::string_view /*presetId*/, uimodel::PreparedLayout const& /*layout*/)
  {
    return false;
  }

  bool FakeLayoutComponentStateStore::removePreset(std::string_view const presetId)
  {
    if (_statePtr->document.preset == presetId)
    {
      _statePtr->document.components.clear();
      return true;
    }

    return false;
  }

  uimodel::LayoutComponentStateDocument const& FakeLayoutComponentStateStore::document() const noexcept
  {
    return _statePtr->document;
  }

  void FakeLayoutComponentStateStore::setDocument(uimodel::LayoutComponentStateDocument doc)
  {
    _statePtr->document = std::move(doc);
  }

  std::int32_t FakeLayoutComponentStateStore::saveCount() const noexcept
  {
    return _statePtr->saveCount;
  }
} // namespace ao::gtk::layout::test
