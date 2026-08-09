// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "layout/runtime/LayoutHost.h"

#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>
#include <ao/uimodel/layout/shell/LayoutRuntimeState.h>

#include <gtkmm/enums.h>

#include <cstdint>
#include <limits>
#include <utility>

namespace ao::gtk::layout
{
  LayoutHost::LayoutHost(ComponentRegistry const& registry)
    : _runtime{registry}
  {
    set_orientation(Gtk::Orientation::VERTICAL);
  }

  Result<LayoutHost::PreparedTree> LayoutHost::prepare(LayoutBuildContext const& ctx,
                                                       uimodel::PreparedLayout const& layout)
  {
    if (ctx.runtimeState.componentStateGeneration == std::numeric_limits<std::uint64_t>::max())
    {
      return makeError(Error::Code::ResourceExhausted, "Layout component-state generation is exhausted");
    }

    auto const nextGeneration = ctx.runtimeState.componentStateGeneration + 1;
    auto buildContext = ctx;
    buildContext.buildState.overrideGeneration(nextGeneration);

    auto rootComponentPtr = _runtime.build(buildContext, layout);

    AO_INVARIANT(rootComponentPtr, "Layout component factory returned no root component");

    auto& activeWidget = rootComponentPtr->widget();
    activeWidget.set_hexpand(true);
    activeWidget.set_vexpand(true);
    return PreparedTree{std::move(rootComponentPtr), nextGeneration};
  }

  void LayoutHost::commit(uimodel::LayoutRuntimeState& runtimeState, PreparedTree prepared)
  {
    // Invalidate pending writes before the retiring generation is detached or destroyed.
    runtimeState.componentStateGeneration = prepared._componentStateGeneration;
    clearLayout();

    _activeComponentPtr = std::move(prepared._rootComponentPtr);

    if (_activeComponentPtr)
    {
      auto& activeWidget = _activeComponentPtr->widget();
      append(activeWidget);
    }
  }

  void LayoutHost::clearLayout()
  {
    if (_activeComponentPtr)
    {
      remove(_activeComponentPtr->widget());
      _activeComponentPtr.reset();
    }
  }
} // namespace ao::gtk::layout
