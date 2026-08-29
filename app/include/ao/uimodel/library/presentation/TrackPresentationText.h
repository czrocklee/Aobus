// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/TrackField.h>
#include <ao/rt/completion/CompletionItem.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ao::i18n
{
  class MessageCatalog;
}

namespace ao::rt
{
  enum class MissingTrackValueKind : std::uint8_t;
}

namespace ao::uimodel
{
  struct TrackPresentationText final
  {
    std::string_view label{};
    std::string_view description{};

    bool operator==(TrackPresentationText const&) const = default;
  };

  std::string_view trackFieldLabel(i18n::MessageCatalog const& catalog, rt::TrackField field) noexcept;
  std::string_view trackGroupKeyLabel(i18n::MessageCatalog const& catalog, rt::TrackGroupKey key) noexcept;
  std::string_view missingTrackValueLabel(i18n::MessageCatalog const& catalog, rt::MissingTrackValueKind kind) noexcept;
  std::optional<TrackPresentationText> builtinTrackPresentation(i18n::MessageCatalog const& catalog,
                                                                std::string_view id) noexcept;
  std::string completionDetail(i18n::MessageCatalog const& catalog, rt::CompletionDetail const& detail);
  std::string trackSelectionSummary(i18n::MessageCatalog const& catalog,
                                    std::size_t count,
                                    std::string_view duration = {});
  std::string smartListMembershipEditingText(i18n::MessageCatalog const& catalog,
                                             bool direct,
                                             std::string_view expression = {});
  std::string smartListPreviewStatus(i18n::MessageCatalog const& catalog,
                                     bool expressionValid,
                                     std::size_t count,
                                     bool isAllTracks,
                                     bool localEmpty);
  std::string trackChannelText(i18n::MessageCatalog const& catalog, std::uint8_t channels);
} // namespace ao::uimodel
