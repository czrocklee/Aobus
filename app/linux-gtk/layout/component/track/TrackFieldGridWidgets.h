// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <glibmm/ustring.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/grid.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/widget.h>
#include <sigc++/signal.h>

#include <cstdint>

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

  class FieldInlineEditor final : public Gtk::Widget
  {
  public:
    FieldInlineEditor();
    ~FieldInlineEditor() override;

    FieldInlineEditor(FieldInlineEditor const&) = delete;
    FieldInlineEditor& operator=(FieldInlineEditor const&) = delete;
    FieldInlineEditor(FieldInlineEditor&&) = delete;
    FieldInlineEditor& operator=(FieldInlineEditor&&) = delete;

    void setText(Glib::ustring const& text);
    Glib::ustring getText() const;

    void setEditable(bool editable);
    bool getEditable() const;
    bool getEditing() const;

    void startEditing();
    void stopEditing(bool commit);

    sigc::signal<void()>& signalEditingChanged();
    sigc::signal<void()>& signalEditingCanceled();

    void removeMaxWidthConstraint();

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
    Gtk::Widget& visibleChild();
    Gtk::Widget const& visibleChild() const;

    std::int32_t widthForVisibleChild(std::int32_t width) const;
    std::int32_t displayLabelMinimumWidth() const;

    Gtk::Label _displayLabel;
    Gtk::Entry _entry;
    Glib::ustring _text;
    sigc::signal<void()> _editingChanged;
    sigc::signal<void()> _editingCanceled;
    bool _editable = false;
    bool _editing = false;
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

  class FieldValueWrapper final : public Gtk::Widget
  {
  public:
    FieldValueWrapper(Gtk::Widget& valueWidget,
                      bool editable,
                      bool technical = false,
                      bool showEditHint = true,
                      bool propagateNaturalWidth = false);
    ~FieldValueWrapper() override;

    FieldValueWrapper(FieldValueWrapper const&) = delete;
    FieldValueWrapper& operator=(FieldValueWrapper const&) = delete;
    FieldValueWrapper(FieldValueWrapper&&) = delete;
    FieldValueWrapper& operator=(FieldValueWrapper&&) = delete;

    void updateHover(bool hovered);

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
    Gtk::Widget& _valueWidget;
    Gtk::Image _editHint;
    bool _editable;
    bool _showEditHint;
    bool _propagateNaturalWidth;
  };

  class CompressibleActionRow final : public Gtk::Widget
  {
  public:
    CompressibleActionRow(Gtk::Widget& content, Gtk::Widget& action, std::int32_t spacing);
    ~CompressibleActionRow() override;

    CompressibleActionRow(CompressibleActionRow const&) = delete;
    CompressibleActionRow& operator=(CompressibleActionRow const&) = delete;
    CompressibleActionRow(CompressibleActionRow&&) = delete;
    CompressibleActionRow& operator=(CompressibleActionRow&&) = delete;

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
    std::int32_t actionNaturalWidth() const;
    std::int32_t spacingForAction(std::int32_t actionWidth) const;

    Gtk::Widget& _content;
    Gtk::Widget& _action;
    std::int32_t _spacing;
  };
} // namespace ao::gtk::layout::track_field_grid
