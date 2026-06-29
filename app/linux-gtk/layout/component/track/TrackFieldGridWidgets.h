// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/rt/completion/CompletionResult.h>

#include <glibmm/ustring.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/label.h>
#include <gtkmm/stack.h>
#include <gtkmm/widget.h>
#include <gtkmm/window.h>
#include <sigc++/signal.h>

#include <cstdint>
#include <memory>

namespace Gtk
{
  class Grid;
}

namespace ao::gtk
{
  class EntryCompletionController;
}

namespace ao::gtk::layout::track_field_grid
{
  constexpr std::int32_t kDefaultFieldRowHeight = 28;

  enum class FixedHeightMinimum : std::uint8_t
  {
    Fixed,
    Zero
  };

  class ConstrainedGridBox final : public Gtk::Widget
  {
  public:
    ConstrainedGridBox();
    ~ConstrainedGridBox() override;

    ConstrainedGridBox(ConstrainedGridBox const&) = delete;
    ConstrainedGridBox& operator=(ConstrainedGridBox const&) = delete;
    ConstrainedGridBox(ConstrainedGridBox&&) = delete;
    ConstrainedGridBox& operator=(ConstrainedGridBox&&) = delete;

    void setGrid(Gtk::Grid& grid);

  protected:
    Gtk::SizeRequestMode get_request_mode_vfunc() const override;

    void measure_vfunc(Gtk::Orientation orientation,
                       int forSize,
                       int& minimum,
                       int& natural,
                       int& minimumBaseline,
                       int& naturalBaseline) const override;

    void size_allocate_vfunc(int width, int height, int baseline) override;

  private:
    Gtk::Grid* _grid = nullptr;
    std::int32_t _lastAllocatedWidth = 0;
  };

  class DetailFieldEditor final : public Gtk::Box
  {
  public:
    DetailFieldEditor();
    ~DetailFieldEditor() override;

    DetailFieldEditor(DetailFieldEditor const&) = delete;
    DetailFieldEditor& operator=(DetailFieldEditor const&) = delete;
    DetailFieldEditor(DetailFieldEditor&&) = delete;
    DetailFieldEditor& operator=(DetailFieldEditor&&) = delete;

    void setText(Glib::ustring const& text);
    Glib::ustring getText() const;

    void setEditable(bool editable);
    bool getEditable() const;
    bool getEditing() const;

    void startEditing();
    void stopEditing(bool commit);
    void setCompletionProvider(rt::CompletionProvider provider);

    Gtk::Label& displayLabelForTest() { return _displayLabel; }
    Gtk::Entry& entryForTest() { return _entry; }
    Gtk::Button& editButtonForTest() { return _editButton; }

    sigc::signal<void()>& signalEditStarted();
    sigc::signal<void()>& signalCommitted();
    sigc::signal<void()>& signalCanceled();

    void removeMaxWidthConstraint();

  private:
    void setEntryTextSilently(Glib::ustring const& text);

    Gtk::Stack _stack;
    Gtk::Label _displayLabel;
    Gtk::Entry _entry;
    Gtk::Button _editButton;
    std::unique_ptr<EntryCompletionController> _completionControllerPtr;
    Glib::ustring _text;
    sigc::signal<void()> _editStarted;
    sigc::signal<void()> _committed;
    sigc::signal<void()> _canceled;
    bool _editable = false;
    bool _editing = false;
  };

  class DetailEditCoordinator final
  {
  public:
    explicit DetailEditCoordinator(Gtk::Window& parentWindow);
    ~DetailEditCoordinator();

    DetailEditCoordinator(DetailEditCoordinator const&) = delete;
    DetailEditCoordinator& operator=(DetailEditCoordinator const&) = delete;
    DetailEditCoordinator(DetailEditCoordinator&&) = delete;
    DetailEditCoordinator& operator=(DetailEditCoordinator&&) = delete;

    void registerEditor(DetailFieldEditor& editor);
    void forgetEditor(DetailFieldEditor& editor);

  private:
    static bool isDescendantOf(Gtk::Widget const* widget, Gtk::Widget const& ancestor);

    Gtk::Window& _parentWindow;
    Glib::RefPtr<Gtk::GestureClick> _outsideClickPtr;
    DetailFieldEditor* _activeEditor = nullptr;
  };

  class FixedHeightWidgetSlot final : public Gtk::Widget
  {
  public:
    explicit FixedHeightWidgetSlot(Gtk::Widget& child,
                                   bool expand = true,
                                   bool propagateNatural = true,
                                   std::int32_t height = kDefaultFieldRowHeight,
                                   FixedHeightMinimum minimum = FixedHeightMinimum::Fixed);
    ~FixedHeightWidgetSlot() override;

    FixedHeightWidgetSlot(FixedHeightWidgetSlot const&) = delete;
    FixedHeightWidgetSlot& operator=(FixedHeightWidgetSlot const&) = delete;
    FixedHeightWidgetSlot(FixedHeightWidgetSlot&&) = delete;
    FixedHeightWidgetSlot& operator=(FixedHeightWidgetSlot&&) = delete;

  protected:
    Gtk::SizeRequestMode get_request_mode_vfunc() const override;

    void measure_vfunc(Gtk::Orientation orientation,
                       int forSize,
                       int& minimum,
                       int& natural,
                       int& minimumBaseline,
                       int& naturalBaseline) const override;

    void size_allocate_vfunc(int width, int height, int baseline) override;

  private:
    Gtk::Widget& _child;
    bool _expand = true;
    bool _propagateNatural = true;
    std::int32_t _minimumHeight;
    std::int32_t _height;
  };
} // namespace ao::gtk::layout::track_field_grid
