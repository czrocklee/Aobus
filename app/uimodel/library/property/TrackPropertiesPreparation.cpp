// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/uimodel/library/property/TrackPropertiesPreparation.h>

#include <ao/Contract.h>
#include <ao/CoreIds.h>
#include <ao/Error.h>
#include <ao/uimodel/library/property/TrackPropertiesFormModel.h>
#include <ao/uimodel/library/track/TrackAuthoringSessions.h>

#include <expected>
#include <span>

namespace ao::uimodel
{
  Result<TrackAuthoringSession> prepareTrackProperties(rt::Library& library,
                                                       rt::LibrarySnapshot const& snapshot,
                                                       std::span<TrackId const> const targetIds,
                                                       TrackPropertiesFormSpec const& spec,
                                                       TrackPropertiesFormModel& form)
  {
    auto sessionRes = TrackAuthoringSession::begin(library, targetIds, snapshot);

    if (!sessionRes)
    {
      return sessionRes;
    }

    if (auto res = loadTrackPropertiesFormBaseline(snapshot, sessionRes->targetIds(), spec, form); !res)
    {
      return std::unexpected{res.error()};
    }

    AO_INVARIANT(form.targetCount() == targetIds.size(), "Properties must bind every requested target occurrence");
    return sessionRes;
  }
} // namespace ao::uimodel
