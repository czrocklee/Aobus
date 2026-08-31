// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <giomm/actionmap.h>
#include <giomm/simpleaction.h>
#include <glibmm/refptr.h>
#include <glibmm/ustring.h>
#include <glibmm/variant.h>
#include <sigc++/functors/slot.h>
#include <sigc++/scoped_connection.h>

#include <cstddef>
#include <vector>

namespace ao::gtk
{
  /**
   * Owns actions and activation connections installed into one borrowed action map.
   *
   * The registration must retire before both the action map and every callback
   * target. Retained action references become inert when the registration resets.
   */
  class [[nodiscard]] ActionMapRegistration final
  {
  public:
    using ActivateSlot = sigc::slot<void(Glib::VariantBase const&)>;

    ActionMapRegistration() = default;
    explicit ActionMapRegistration(Gio::ActionMap& actionMap, std::size_t expectedActionCount = 0);
    ~ActionMapRegistration();

    ActionMapRegistration(ActionMapRegistration const&) = delete;
    ActionMapRegistration& operator=(ActionMapRegistration const&) = delete;

    ActionMapRegistration(ActionMapRegistration&& other) noexcept;
    ActionMapRegistration& operator=(ActionMapRegistration&& other) noexcept;

    void add(Glib::RefPtr<Gio::SimpleAction> actionPtr, ActivateSlot const& activate);
    void reset() noexcept;

    bool isCurrent(Glib::RefPtr<Gio::SimpleAction> const& actionPtr) const;

  private:
    struct Entry final
    {
      Glib::ustring actionName;
      Glib::RefPtr<Gio::SimpleAction> actionPtr;
      sigc::scoped_connection activationConnection;
    };

    Gio::ActionMap* _actionMap = nullptr;
    std::vector<Entry> _entries;
  };
} // namespace ao::gtk
