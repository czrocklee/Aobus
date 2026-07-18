// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "common/PopoverAttachment.h"

#include <glibmm/main.h>
#include <gtkmm/popover.h>
#include <gtkmm/widget.h>
#include <sigc++/functors/mem_fun.h>

#include <memory>
#include <utility>

namespace ao::gtk
{
  PopoverAttachment::~PopoverAttachment()
  {
    detach();
  }

  bool PopoverAttachment::hasPopover() const
  {
    return _popoverPtr != nullptr;
  }

  void PopoverAttachment::attach(std::unique_ptr<Gtk::Popover> popoverPtr, Gtk::Widget& anchor)
  {
    detachActivePopover();
    _popoverPtr = std::move(popoverPtr);
    _popoverPtr->set_parent(anchor);
    _closedConnection =
      _popoverPtr->signal_closed().connect(sigc::mem_fun(*this, &PopoverAttachment::retireAfterDispatch));
    _anchorUnmapConnection =
      anchor.signal_unmap().connect(sigc::mem_fun(*this, &PopoverAttachment::retireAfterDispatch));
  }

  void PopoverAttachment::popup()
  {
    if (_popoverPtr)
    {
      _popoverPtr->popup();
    }
  }

  void PopoverAttachment::detach()
  {
    _retirementConnection.disconnect();
    detachActivePopover();
    _retiredPopoverPtrs.clear();
  }

  void PopoverAttachment::retireAfterDispatch()
  {
    _closedConnection.disconnect();
    _anchorUnmapConnection.disconnect();

    if (_popoverPtr->get_parent() != nullptr)
    {
      _popoverPtr->unparent();
    }

    _retiredPopoverPtrs.push_back(std::move(_popoverPtr));

    if (!_retirementConnection.connected())
    {
      _retirementConnection = Glib::signal_idle().connect(
        [this]
        {
          _retiredPopoverPtrs.clear();
          return false;
        });
    }
  }

  void PopoverAttachment::detachActivePopover()
  {
    _closedConnection.disconnect();
    _anchorUnmapConnection.disconnect();

    if (_popoverPtr && _popoverPtr->get_parent() != nullptr)
    {
      _popoverPtr->unparent();
    }

    _popoverPtr.reset();
  }
} // namespace ao::gtk
