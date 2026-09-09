// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include "test/unit/tui/KeymapTestSupport.h"

#include "tui/Keymap.h"
#include <ao/uimodel/input/KeymapModel.h>

namespace ao::tui::test
{
  KeymapPlan const& defaultKeymapPlan()
  {
    static auto const kPlan = KeymapPlan{uimodel::KeymapModel{defaultKeymap()}};
    return kPlan;
  }
} // namespace ao::tui::test
