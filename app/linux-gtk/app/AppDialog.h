// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/enums.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>
#include <sigc++/signal.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk
{
  enum class AppDialogActionRole : std::uint8_t
  {
    Cancel,
    Primary,
  };

  struct AppDialogAction final
  {
    std::string label;
    std::int32_t responseId;
    AppDialogActionRole role;
  };

  /**
   * AppDialog provides a consistent UI/UX for dialogs in Aobus.
   *
   * It uses a Gtk::HeaderBar for actions and provides a Gtk::Dialog-compatible
   * response API to ensure smooth migration.
   */
  class AppDialog : public Gtk::Window
  {
  public:
    AppDialog();
    ~AppDialog() override;

    AppDialog(AppDialog const&) = delete;
    AppDialog& operator=(AppDialog const&) = delete;
    AppDialog(AppDialog&&) = delete;
    AppDialog& operator=(AppDialog&&) = delete;

    /**
     * Emits signal_response with the given ID.
     */
    void response(std::int32_t id);

    /**
     * Compatibility signal that matches Gtk::Dialog's response signal.
     */
    sigc::signal<void(std::int32_t)> signal_response();

    /**
     * Applies standard modal parent behavior used by all Aobus child dialogs.
     */
    void configureForParent(Gtk::Window& parent);

    /**
     * Selects the response button activated by the window default action.
     */
    void setDefaultResponse(std::int32_t responseId);

    /**
     * Emits the given response when the user dismisses the window directly.
     */
    void setCloseResponse(std::int32_t responseId);

    /**
     * Sets the main content widget of the dialog.
     * It will be wrapped in a container with standard dialog padding.
     */
    void setContentWidget(Gtk::Widget& widget);

    /**
     * Adds a primary (suggested) action to the right side of the header bar.
     */
    Gtk::Button* addPrimaryAction(std::string const& label, std::int32_t responseId);

    /**
     * Adds a cancel action to the left side of the header bar.
     */
    Gtk::Button* addCancelAction(std::string const& label, std::int32_t responseId);

    /**
     * Builds a standard message dialog without presenting it.
     */
    static std::shared_ptr<AppDialog> createMessage(Gtk::Window& parent,
                                                    std::string const& title,
                                                    std::string const& message,
                                                    std::vector<AppDialogAction> const& actions,
                                                    std::int32_t defaultResponseId);

    /**
     * Presents a standard message dialog and closes it after the response callback.
     */
    static AppDialog* presentMessage(Gtk::Window& parent,
                                     std::string const& title,
                                     std::string const& message,
                                     std::vector<AppDialogAction> const& actions,
                                     std::int32_t defaultResponseId,
                                     std::function<void(std::int32_t)> onResponse = {});

    /**
     * Provides access to the underlying header bar to add custom widgets.
     */
    Gtk::HeaderBar& headerBar() { return _headerBar; }

  private:
    Gtk::Button* addAction(std::string const& label, std::int32_t responseId, AppDialogActionRole role);
    void applyDefaultResponse();
    static void configureMessageDialog(AppDialog& dialog,
                                       Gtk::Window& parent,
                                       std::string const& title,
                                       std::string const& message,
                                       std::vector<AppDialogAction> const& actions,
                                       std::int32_t defaultResponseId);

    Gtk::HeaderBar _headerBar;
    Gtk::Box _rootBox{Gtk::Orientation::VERTICAL};
    Gtk::Box _contentWrapper;
    Gtk::Box _actionBar{Gtk::Orientation::HORIZONTAL};
    Gtk::Box _startActions{Gtk::Orientation::HORIZONTAL};
    Gtk::Box _endActions{Gtk::Orientation::HORIZONTAL};
    sigc::signal<void(std::int32_t)> _signalResponse;
    std::vector<std::pair<std::int32_t, Gtk::Button*>> _responseButtons;
    std::optional<std::int32_t> _optDefaultResponseId;
    std::optional<std::int32_t> _optCloseResponseId;
    bool _responseInProgress = false;
  };
} // namespace ao::gtk
