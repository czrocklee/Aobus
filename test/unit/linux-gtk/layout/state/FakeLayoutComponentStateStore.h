// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/uimodel/layout/component/LayoutComponentCatalog.h>
#include <ao/uimodel/layout/component/LayoutComponentStateStore.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace ao::uimodel
{
  class PreparedLayout;
}

namespace ao::gtk::layout::test
{
  class FakeLayoutComponentStateStore final : public uimodel::LayoutComponentStateStore
  {
  public:
    FakeLayoutComponentStateStore();
    ~FakeLayoutComponentStateStore() override;

    FakeLayoutComponentStateStore(FakeLayoutComponentStateStore const&) = delete;
    FakeLayoutComponentStateStore& operator=(FakeLayoutComponentStateStore const&) = delete;
    FakeLayoutComponentStateStore(FakeLayoutComponentStateStore&&) = delete;
    FakeLayoutComponentStateStore& operator=(FakeLayoutComponentStateStore&&) = delete;

    std::optional<uimodel::LayoutComponentStateDocument> load(std::string_view presetId) const override;
    void save(std::string_view presetId, uimodel::LayoutComponentStateDocument const& doc) override;
    bool prune(std::string_view presetId,
               uimodel::PreparedLayout const& layout,
               uimodel::LayoutComponentCatalog const& catalog) override;
    bool removePreset(std::string_view presetId) override;

    uimodel::LayoutComponentStateDocument const& document() const noexcept;
    void setDocument(uimodel::LayoutComponentStateDocument doc);
    std::int32_t saveCount() const noexcept;

  private:
    struct State;
    std::unique_ptr<State> _statePtr;
  };
} // namespace ao::gtk::layout::test
