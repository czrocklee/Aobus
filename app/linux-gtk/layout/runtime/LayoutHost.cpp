// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 Aobus Contributors

#include "layout/runtime/LayoutHost.h"

#include "layout/runtime/LayoutBuildContext.h"
#include "layout/runtime/LayoutComponent.h"
#include "track/TrackPageHost.h"
#include <ao/Contract.h>
#include <ao/Error.h>
#include <ao/uimodel/layout/document/LayoutPreparation.h>

#include <gtkmm/box.h>
#include <gtkmm/enums.h>
#include <gtkmm/stack.h>
#include <gtkmm/widget.h>
#include <sigc++/adaptors/track_obj.h>

#include <exception>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

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

  void SharedWidgetHandoff::transfer(Gtk::Widget& widget, Gtk::Box& destination)
  {
    reserve(Transfer{.widget = &widget}, destination);
  }

  void SharedWidgetHandoff::transferTrackStack(TrackPageHost& trackPageHost,
                                               Gtk::Box& destination,
                                               TrackStackPresentation const presentation)
  {
    reserve(Transfer{.widget = &trackPageHost.stack(), .trackPageHost = &trackPageHost, .presentation = presentation},
            destination);
  }

  void SharedWidgetHandoff::reserve(Transfer pending, Gtk::Box& destination)
  {
    AO_EXPECTS(destination.get_first_child() == nullptr, "A shared widget reserves the first child slot");
    pending.resolveDestination = sigc::track_object([&destination] { return &destination; }, destination);
    _transfers.push_back(std::move(pending));
  }

  void SharedWidgetHandoff::validateReservations()
  {
    // A container can replace a child (and its entire subtree) during the build.
    // Only the reservations whose wrappers survived can conflict.
    std::erase_if(_transfers, [](Transfer const& transfer) { return transfer.resolveDestination.empty(); });
    auto widgets = std::unordered_set<Gtk::Widget*>{};
    auto destinations = std::unordered_set<Gtk::Box*>{};

    for (auto const& transfer : _transfers)
    {
      if (!widgets.insert(transfer.widget).second)
      {
        throwLayoutBuildError("A shell-owned widget may appear only once in a layout candidate");
      }

      if (auto* const parent = transfer.widget->get_parent();
          parent != nullptr && dynamic_cast<Gtk::Box*>(parent) == nullptr)
      {
        throwLayoutBuildError("A shell-owned layout widget must be parented by Gtk::Box");
      }

      AO_EXPECTS(destinations.insert(transfer.resolveDestination()).second,
                 "Each candidate box can reserve only one shared widget slot");
    }
  }

  void SharedWidgetHandoff::attach()
  {
    for (auto const& transfer : _transfers)
    {
      auto* const destination = transfer.resolveDestination();

      if (destination == nullptr)
      {
        continue;
      }

      // Destroying the old wrappers detaches their shared children. Also
      // support a widget that was previously hosted in another Gtk::Box.
      if (auto* const parent = transfer.widget->get_parent(); parent != nullptr)
      {
        auto* const box = dynamic_cast<Gtk::Box*>(parent);
        AO_EXPECTS(box != nullptr, "A shared widget changed to an unsupported parent before publication");
        box->remove(*transfer.widget);
      }

      // The workspace adds the handle and revealer after reserving this slot.
      destination->prepend(*transfer.widget);

      if (transfer.trackPageHost == nullptr)
      {
        continue;
      }

      if (auto const optStyle = transfer.presentation.optGroupCoverPlaceholderStyle; optStyle)
      {
        transfer.trackPageHost->setGroupCoverPlaceholderStyle(*optStyle);
      }

      if (transfer.presentation.forceExpand)
      {
        auto& stack = transfer.trackPageHost->stack();
        stack.set_hexpand(true);
        stack.set_vexpand(true);
      }
    }
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
      handoffPtr->validateReservations();
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
    return PreparedTree{
      *this, ctx.session, std::move(rootComponentPtr), std::move(handoffPtr), ctx.buildSnapshot.generation()};
  }

  void LayoutHost::commit(PreparedTree prepared)
  {
    AO_EXPECTS(prepared._host == this && prepared._rootComponentPtr && prepared._sharedWidgetHandoffPtr,
               "A layout host can only publish its own, unconsumed candidate");
    AO_EXPECTS(
      prepared.generation() == prepared._session->generation() && prepared.generation() > _lastPublishedGeneration,
      "A candidate must publish exactly once for the current session generation");
    _lastPublishedGeneration = prepared.generation();

    clearLayout();
    _activeComponentPtr = std::move(prepared._rootComponentPtr);
    prepared._sharedWidgetHandoffPtr->attach();
    append(_activeComponentPtr->widget());
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
