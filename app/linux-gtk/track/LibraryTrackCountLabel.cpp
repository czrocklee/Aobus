// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "track/LibraryTrackCountLabel.h"

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/TrackEditScript.h>
#include <ao/rt/source/TrackSource.h>
#include <ao/rt/source/TrackSourceDelta.h>
#include <ao/rt/source/TrackSourceLease.h>
#include <ao/uimodel/presentation/PresentationTextCatalog.h>

#include <algorithm>
#include <utility>
#include <variant>

namespace ao::gtk
{
  LibraryTrackCountLabel::LibraryTrackCountLabel(rt::TrackSourceLease sourceLease,
                                                 uimodel::PresentationTextCatalog textCatalog)
    : _sourceLease{std::move(sourceLease)}, _textCatalog{std::move(textCatalog)}
  {
    _label.add_css_class("dim-label");
    _sourceSubscription =
      _sourceLease->subscribe([this](rt::TrackSourceDelta const& batch) { handleSourceBatch(batch); });

    updateCount();
  }

  LibraryTrackCountLabel::~LibraryTrackCountLabel() = default;

  void LibraryTrackCountLabel::handleSourceBatch(rt::TrackSourceDelta const& message)
  {
    if (std::holds_alternative<rt::SourceInvalidated>(message))
    {
      _sourceSubscription.reset();
      return;
    }

    auto const* script = std::get_if<rt::delta::RegularTrackEditScript>(&message);

    if (std::holds_alternative<rt::SourceReset>(message) ||
        (script != nullptr && std::ranges::any_of(script->edits,
                                                  [](rt::delta::RegularTrackEdit const& edit)
                                                  {
                                                    return std::holds_alternative<rt::delta::InsertRange>(edit) ||
                                                           std::holds_alternative<rt::delta::RemoveRange>(edit);
                                                  })))
    {
      updateCount();
    }
  }

  void LibraryTrackCountLabel::updateCount()
  {
    _label.set_text(_textCatalog.format(i18n::MessageId::TrackCount, {{"count", _sourceLease->size()}}));
  }
} // namespace ao::gtk
