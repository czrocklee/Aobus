// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "common/ActionMapRegistration.h"

#include <ao/Contract.h>

#include <giomm/action.h>
#include <giomm/actionmap.h>
#include <giomm/simpleaction.h>
#include <glibmm/refptr.h>

#include <cstddef>
#include <memory>
#include <utility>

namespace ao::gtk
{
  namespace
  {
    bool isSameAction(Glib::RefPtr<Gio::Action> const& currentActionPtr,
                      Glib::RefPtr<Gio::SimpleAction> const& installedActionPtr) noexcept
    {
      auto const installedBasePtr = std::static_pointer_cast<Gio::Action>(installedActionPtr);
      return currentActionPtr && installedBasePtr && currentActionPtr->gobj() == installedBasePtr->gobj();
    }
  } // namespace

  ActionMapRegistration::ActionMapRegistration(Gio::ActionMap& actionMap, std::size_t const expectedActionCount)
    : _actionMap{&actionMap}
  {
    _entries.reserve(expectedActionCount);
  }

  ActionMapRegistration::~ActionMapRegistration()
  {
    reset();
  }

  ActionMapRegistration::ActionMapRegistration(ActionMapRegistration&& other) noexcept
    : _actionMap{std::exchange(other._actionMap, nullptr)}, _entries{std::move(other._entries)}
  {
  }

  ActionMapRegistration& ActionMapRegistration::operator=(ActionMapRegistration&& other) noexcept
  {
    if (this != &other)
    {
      reset();
      _actionMap = std::exchange(other._actionMap, nullptr);
      _entries = std::move(other._entries);
    }

    return *this;
  }

  void ActionMapRegistration::add(Glib::RefPtr<Gio::SimpleAction> actionPtr, ActivateSlot const& activate)
  {
    AO_EXPECTS(_actionMap != nullptr, "Cannot add an action through an inactive registration");
    AO_EXPECTS(actionPtr != nullptr, "Cannot register a null action");

    auto actionName = actionPtr->get_name();
    auto activationConnection = sigc::scoped_connection{actionPtr->signal_activate().connect(activate)};
    _entries.push_back(Entry{.actionName = std::move(actionName),
                             .actionPtr = actionPtr,
                             .activationConnection = std::move(activationConnection)});
    _actionMap->add_action(actionPtr);
  }

  void ActionMapRegistration::reset() noexcept
  {
    auto* const actionMap = std::exchange(_actionMap, nullptr);
    auto entries = std::move(_entries);
    _entries.clear();

    for (auto& entry : entries)
    {
      entry.activationConnection.disconnect();
    }

    if (actionMap != nullptr)
    {
      for (auto const& entry : entries)
      {
        auto const currentActionPtr = actionMap->lookup_action(entry.actionName);

        if (isSameAction(currentActionPtr, entry.actionPtr))
        {
          actionMap->remove_action(entry.actionName);
        }
      }
    }
  }

  bool ActionMapRegistration::isCurrent(Glib::RefPtr<Gio::SimpleAction> const& actionPtr) const
  {
    if (_actionMap == nullptr || actionPtr == nullptr)
    {
      return false;
    }

    return isSameAction(_actionMap->lookup_action(actionPtr->get_name()), actionPtr);
  }
} // namespace ao::gtk
