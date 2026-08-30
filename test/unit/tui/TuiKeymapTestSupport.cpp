// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/tui/TuiKeymapTestSupport.h"

#include "tui/TuiKeymap.h"
#include <ao/uimodel/input/KeymapModel.h>

namespace ao::tui::test
{
  TuiKeymapPlan const& defaultTuiKeymapPlan()
  {
    static auto const plan = TuiKeymapPlan{uimodel::KeymapModel{tuiDefaultKeymap()}};
    return plan;
  }
} // namespace ao::tui::test
