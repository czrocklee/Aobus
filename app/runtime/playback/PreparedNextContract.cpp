// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "runtime/playback/PreparedNextContract.h"

#include <ao/Contract.h>

#include <source_location>

namespace ao::rt::detail
{
  void expectPreparedNextSlotAvailable(bool const hasActivePreparedToken, std::source_location const location)
  {
    if (hasActivePreparedToken) [[unlikely]]
    {
      AO_EXPECTS_AT(
        location, !hasActivePreparedToken, "Prepared-next request must be cleared before preparing a replacement");
    }
  }
} // namespace ao::rt::detail
