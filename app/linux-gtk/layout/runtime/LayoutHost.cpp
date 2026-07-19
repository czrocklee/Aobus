// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "layout/runtime/LayoutHost.h"

#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "layout/runtime/LayoutRuntimeState.h"
#include <ao/Error.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>

#include <gtkmm/enums.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk::layout
{
  namespace
  {
    std::string boundedExceptionMessage(std::string_view message)
    {
      constexpr std::size_t kMaximumBytes = 160;

      if (message.size() <= kMaximumBytes)
      {
        return std::string{message};
      }

      constexpr auto kSuffix = std::string_view{"..."};
      auto result = std::string{message.substr(0, kMaximumBytes - kSuffix.size())};
      result += kSuffix;
      return result;
    }
  } // namespace

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

    try
    {
      auto rootComponentPtr = _runtime.build(buildContext, layout);

      if (!rootComponentPtr)
      {
        return makeError(Error::Code::InvalidState, "Layout component factory returned no root component");
      }

      auto& activeWidget = rootComponentPtr->widget();
      activeWidget.set_hexpand(true);
      activeWidget.set_vexpand(true);
      return PreparedTree{std::move(rootComponentPtr), nextGeneration};
    }
    catch (std::bad_alloc const&)
    {
      return makeError(Error::Code::ResourceExhausted, "Insufficient memory to build GTK layout tree");
    }
    catch (std::exception const& error)
    {
      return makeError(Error::Code::InvalidState,
                       std::format("Failed to build GTK layout tree: {}", boundedExceptionMessage(error.what())));
    }
    catch (...)
    {
      return makeError(Error::Code::InvalidState, "Failed to build GTK layout tree with an unknown exception");
    }
  }

  void LayoutHost::commit(LayoutRuntimeState& runtimeState, PreparedTree prepared)
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
