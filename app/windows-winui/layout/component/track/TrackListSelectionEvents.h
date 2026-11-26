// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#pragma push_macro("GetCurrentTime")
#include <unknwn.h>
#pragma pop_macro("GetCurrentTime")

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Data.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <cstdint>
#include <memory>

namespace ao::uimodel
{
  class ShellGenerationGate;
}

namespace ao::winui::layout
{
  /// Compare committed range snapshots, never individual ISelectionInfo mutations during restoration.
  class TrackListSelectionEvents final
  {
  public:
    using Ranges =
      winrt::Windows::Foundation::Collections::IVectorView<winrt::Microsoft::UI::Xaml::Data::ItemIndexRange>;

    struct Revision final
    {
      std::weak_ptr<std::uint64_t const> counterPtr;
      std::uint64_t serial;

      bool isAlive() const noexcept { return !counterPtr.expired(); }
      bool isCurrent() const noexcept;
    };

    Revision revision() const noexcept { return {.counterPtr = _revisionPtr, .serial = *_revisionPtr}; }
    void interrupt() noexcept { ++*_revisionPtr; }
    void commit(winrt::Microsoft::UI::Xaml::Controls::ListView const& rows,
                Ranges const& ranges,
                std::weak_ptr<uimodel::ShellGenerationGate> const& gatePtr);

  private:
    // Native peer calls can reenter XAML. A superseded or retired publisher must stop its batch.
    std::shared_ptr<std::uint64_t> _revisionPtr = std::make_shared<std::uint64_t>(0);
    winrt::Windows::Foundation::IInspectable _source{nullptr};
    Ranges _ranges{nullptr};
  };
} // namespace ao::winui::layout
