// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "layout/runtime/LayoutHost.h"

#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>

#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/widget.h>

#include <algorithm>
#include <exception>
#include <format>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace ao::gtk::layout
{
  namespace
  {
    [[noreturn]] void throwLayoutBuildError(std::string_view const message)
    {
      AO_EXCEPTION_CARRIER(PrivateErrorTransport);
      throw std::logic_error{std::string{message}};
    }
  } // namespace

  SharedWidgetHandoff::~SharedWidgetHandoff()
  {
    if (_committed)
    {
      return;
    }

    for (auto& transfer : _transfers | std::views::reverse)
    {
      if (transfer.widget->get_parent() != nullptr)
      {
        transfer.widget->unparent();
      }

      if (transfer.previousParent == nullptr)
      {
        continue;
      }

      if (transfer.previousSibling != nullptr && transfer.previousSibling->get_parent() == transfer.previousParent)
      {
        transfer.previousParent->insert_child_after(*transfer.widget, *transfer.previousSibling);
      }
      else
      {
        transfer.previousParent->prepend(*transfer.widget);
      }
    }
  }

  void SharedWidgetHandoff::transfer(Gtk::Widget& widget, Gtk::Box& destination)
  {
    if (std::ranges::any_of(_transfers, [&widget](Transfer const& transfer) { return transfer.widget == &widget; }))
    {
      throwLayoutBuildError("A shell-owned widget may appear only once in a layout candidate");
    }

    auto* const previousWidgetParent = widget.get_parent();
    auto* const previousParent = dynamic_cast<Gtk::Box*>(previousWidgetParent);

    if (previousWidgetParent != nullptr && previousParent == nullptr)
    {
      throwLayoutBuildError("A shell-owned layout widget must be parented by Gtk::Box");
    }

    auto* const previousSibling = widget.get_prev_sibling();
    _transfers.push_back({.widget = &widget, .previousParent = previousParent, .previousSibling = previousSibling});

    if (previousWidgetParent != nullptr)
    {
      widget.unparent();
    }

    destination.append(widget);
  }

  void SharedWidgetHandoff::commit() noexcept
  {
    _committed = true;
  }

  LayoutHost::LayoutHost(ComponentRegistry const& registry)
    : _runtime{registry}
  {
    set_orientation(Gtk::Orientation::VERTICAL);
  }

  Result<LayoutHost::PreparedTree> LayoutHost::prepare(LayoutBuildContext const& ctx,
                                                       uimodel::PreparedLayout const& layout)
  {
    auto buildContext = ctx;
    auto handoffPtr = std::make_unique<SharedWidgetHandoff>();
    buildContext.sharedWidgetHandoff = handoffPtr.get();
    auto rootComponentPtr = std::unique_ptr<LayoutComponent>{};

    try
    {
      rootComponentPtr = _runtime.build(buildContext, layout);
    }
    catch (std::exception const& ex)
    {
      AO_AUDITED_CATCH(DiagnosticFallback);
      return makeError(Error::Code::InitFailed, std::format("Failed to build GTK layout candidate: {}", ex.what()));
    }

    if (!rootComponentPtr)
    {
      return makeError(Error::Code::InitFailed, "Layout component factory returned no root component");
    }

    auto& activeWidget = rootComponentPtr->widget();
    activeWidget.set_hexpand(true);
    activeWidget.set_vexpand(true);
    return PreparedTree{std::move(rootComponentPtr), std::move(handoffPtr), ctx.buildSnapshot.generation()};
  }

  void LayoutHost::commit(PreparedTree prepared)
  {
    prepared._sharedWidgetHandoffPtr->commit();
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
