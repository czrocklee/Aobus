// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "tag/TagEditor.h"

#include "common/DismissController.h"
#include "layout/LayoutConstants.h"
#include <ao/Type.h>
#include <ao/rt/library/Library.h>
#include <ao/rt/library/LibraryReader.h>

#include <gdk/gdkkeysyms.h>
#include <gdkmm/enums.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/entry.h>
#include <gtkmm/enums.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/object.h>
#include <pangomm/layout.h>
#include <sigc++/functors/mem_fun.h>
#include <sigc++/signal.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ao::gtk
{
  namespace
  {
    constexpr int kChipSpacing = layout::kSpacingSmall; // 4
    constexpr std::size_t kMaxAvailableTags = 50;

    struct MeasureResult final
    {
      std::int32_t minimum = 0;
      std::int32_t natural = 0;
    };

    MeasureResult measureWidget(Gtk::Widget const& widget, Gtk::Orientation orientation, std::int32_t forSize)
    {
      auto result = MeasureResult{};
      std::int32_t minimumBaseline = -1;
      std::int32_t naturalBaseline = -1;
      widget.measure(orientation, forSize, result.minimum, result.natural, minimumBaseline, naturalBaseline);
      return result;
    }

    std::string toLower(std::string value)
    {
      std::ranges::transform(value, value.begin(), [](unsigned char ch) { return std::tolower(ch); });
      return value;
    }

    struct FlowSize final
    {
      std::int32_t naturalWidth = 0;
      std::int32_t height = 0;
    };

    // A self-contained left-to-right wrapping flow over a widget's visible children. Unlike
    // GtkFlowBox (a grid that sizes every column to its widest child, so a chip inherits the width
    // of whatever shares its column), each child is allocated exactly its own natural width with a
    // fixed gap; a child wider than the line is clamped to the line width so its label can ellipsize.
    // Rows are sized to their tallest child and children centre vertically (via their valign), so a
    // taller neighbour (the open entry) never stretches the chips beside it.
    //
    // `place(child, x, y, width, height)` is invoked for every laid-out child; pass a no-op to only
    // measure. Returns the natural (single longest row) width and the total wrapped height.
    template<typename WidgetT, typename PlaceFn>
    FlowSize layoutFlow(WidgetT* first, std::int32_t availableWidth, std::int32_t spacing, PlaceFn place)
    {
      struct Item final
      {
        WidgetT* widget;
        std::int32_t x;
        std::int32_t width;
        std::int32_t row;
      };

      auto items = std::vector<Item>{};
      auto rowHeights = std::vector<std::int32_t>{0};
      std::int32_t cursorX = 0;
      std::int32_t row = 0;

      for (auto* child = first; child != nullptr; child = child->get_next_sibling())
      {
        if (!child->get_visible())
        {
          continue;
        }

        auto const horizontal = measureWidget(*child, Gtk::Orientation::HORIZONTAL, -1);
        auto const naturalWidth = horizontal.natural;

        if (cursorX > 0 && cursorX + naturalWidth > availableWidth)
        {
          cursorX = 0;
          ++row;
          rowHeights.push_back(0);
        }

        // Natural width, clamped to the line but never below the child's own minimum (a child wider
        // than the line keeps its minimum and is clipped by the editor's overflow).
        auto const allocWidth = std::max(horizontal.minimum, std::min(naturalWidth, std::max(availableWidth, 0)));
        auto const height = measureWidget(*child, Gtk::Orientation::VERTICAL, allocWidth).natural;

        items.push_back({child, cursorX, allocWidth, row});
        rowHeights[static_cast<std::size_t>(row)] = std::max(rowHeights[static_cast<std::size_t>(row)], height);
        cursorX += allocWidth + spacing;
      }

      auto rowTops = std::vector<std::int32_t>(rowHeights.size(), 0);
      std::int32_t yPos = 0;

      for (std::size_t rowIdx = 0; rowIdx < rowHeights.size(); ++rowIdx)
      {
        if (rowIdx > 0)
        {
          yPos += spacing;
        }

        rowTops[rowIdx] = yPos;
        yPos += rowHeights[rowIdx];
      }

      auto size = FlowSize{.naturalWidth = 0, .height = yPos};

      for (auto const& item : items)
      {
        auto const rowIdx = static_cast<std::size_t>(item.row);
        place(item.widget, item.x, rowTops[rowIdx], item.width, rowHeights[rowIdx]);
        size.naturalWidth = std::max(size.naturalWidth, item.x + item.width);
      }

      return size;
    }

    // A width that no realistic chip run reaches, so layoutFlow never wraps — used to measure the
    // single-row natural width/height when the caller imposes no width constraint.
    constexpr std::int32_t kUnconstrainedWidth = 1 << 24;

    // Inter-chip gap used by the wrapping flow (both the horizontal gap between chips and the
    // vertical gap between wrapped rows). This is theme-aware: the airy "modern" theme spaces chips
    // out generously, while "classic" stays dense. The theme class lives on the toplevel window, so
    // we read it from the widget's root; before the widget is rooted (e.g. headless tests) we fall
    // back to the dense gap. Distinct from kChipSpacing, which is the intra-chip composition gap
    // (label-to-remove-button etc.) and must not grow with the theme.
    std::int32_t chipFlowGap(Gtk::Widget const& widget)
    {
      if (auto const* const root = widget.get_root(); root != nullptr)
      {
        // Mirrors themeCssClass(ThemePresetId::Modern); kept as a literal to avoid per-measure churn.
        if (auto const* rootWidget = dynamic_cast<Gtk::Widget const*>(root);
            rootWidget != nullptr && rootWidget->has_css_class("ao-theme-modern"))
        {
          return layout::kSpacingLarge; // 8 — generous breathing for the floating-card theme
        }
      }

      return kChipSpacing; // 4 — classic density (and the default before the widget is rooted)
    }

    // Type A: an existing tag on the selected tracks. Solid fill, with an explicit remove button.
    class TagChip final : public Gtk::Box
    {
    public:
      explicit TagChip(std::string const& tag)
        : Gtk::Box{Gtk::Orientation::HORIZONTAL, kChipSpacing}, _label{tag}
      {
        set_overflow(Gtk::Overflow::HIDDEN);

        _label.set_ellipsize(Pango::EllipsizeMode::END);
        _label.set_hexpand(true);
        _label.set_margin_start(kLabelMarginStart);
        _label.add_css_class("ao-tag-chip-label");
        append(_label);

        _removeBtn.set_icon_name("window-close-symbolic");
        _removeBtn.set_has_frame(false);
        _removeBtn.set_size_request(kRemoveBtnSize, kRemoveBtnSize);
        _removeBtn.set_valign(Gtk::Align::CENTER);
        _removeBtn.set_margin_end(kRemoveBtnMarginEnd);
        _removeBtn.add_css_class("ao-tag-chip-remove");
        append(_removeBtn);

        add_css_class("ao-tag-chip");
        add_css_class("ao-tag-chip-current");

        // Centre within the flow row's height so a taller neighbour (the open inline add entry)
        // never stretches the chip vertically.
        set_valign(Gtk::Align::CENTER);
      }

      ~TagChip() override = default;

      TagChip(TagChip const&) = delete;
      TagChip& operator=(TagChip const&) = delete;
      TagChip(TagChip&&) = delete;
      TagChip& operator=(TagChip&&) = delete;

      auto signal_remove() { return _removeBtn.signal_clicked(); }

    private:
      static constexpr std::int32_t kLabelMarginStart = 8;
      static constexpr std::int32_t kRemoveBtnMarginEnd = 4;
      static constexpr std::int32_t kRemoveBtnSize = 20;

      Gtk::Label _label;
      Gtk::Button _removeBtn;
    };

    // Type B: a suggested/high-frequency tag not yet on the selection. Outlined, adds on click.
    class AvailableTagChip final : public Gtk::Button
    {
    public:
      explicit AvailableTagChip(std::string const& tag)
        : _tag{tag}, _label{tag}
      {
        _icon.set_from_icon_name("list-add-symbolic");
        _icon.set_pixel_size(kAddIconSize); // default 16px reads too heavy next to the small label
        _icon.set_valign(Gtk::Align::CENTER);
        _box.set_spacing(kChipSpacing);
        _box.append(_icon);
        _box.append(_label);
        set_child(_box);
        set_has_frame(false);
        add_css_class("ao-tag-chip");
        add_css_class("ao-tag-chip-suggested");
        set_valign(Gtk::Align::CENTER);
      }

      std::string const& getTag() const { return _tag; }

    private:
      static constexpr int kAddIconSize = 12;

      std::string _tag;
      Gtk::Box _box{Gtk::Orientation::HORIZONTAL};
      Gtk::Image _icon;
      Gtk::Label _label;
    };
  } // namespace

  // Type C: the inline add trigger. A lightweight button that swaps in place for a text entry on
  // demand, committing on Enter (and staying open for rapid adds) while Escape or focus-out
  // dismiss it without committing.
  class AddTagTrigger final : public Gtk::Box
  {
  public:
    using SubmitSignal = sigc::signal<void(std::string const&)>;
    using FilterSignal = sigc::signal<void()>;

    AddTagTrigger()
      : Gtk::Box{Gtk::Orientation::HORIZONTAL, 0}
    {
      add_css_class("ao-tag-add");
      set_valign(Gtk::Align::CENTER);

      // A bare "Add…" label (no "+") keeps this reading as an action and avoids echoing the "+"
      // glyph the suggested chips use; the frameless ghost styling sets it apart from the pills.
      _button.set_label("Add…");
      _button.set_has_frame(false);
      _button.add_css_class("ao-tag-add-trigger");
      _button.signal_clicked().connect(sigc::mem_fun(*this, &AddTagTrigger::openEntry));
      append(_button);

      _entry.set_placeholder_text("Add tag…");
      _entry.add_css_class("ao-tags-entry");
      // A comfortable typing width; longer input scrolls within the entry. The flow layout sizes
      // each child independently, so a wider entry no longer stretches neighbouring chips (the
      // reason this was previously kept artificially narrow under GtkFlowBox's grid columns).
      _entry.set_width_chars(kEntryWidthChars);
      _entry.set_max_width_chars(kEntryMaxWidthChars);
      _entry.set_visible(false);
      _entry.signal_activate().connect(sigc::mem_fun(*this, &AddTagTrigger::onActivate));
      _entry.signal_changed().connect([this] { _filterChanged.emit(); });
      auto focusControllerPtr = Gtk::EventControllerFocus::create();
      focusControllerPtr->signal_leave().connect(sigc::mem_fun(*this, &AddTagTrigger::onFocusLeave));
      _entry.add_controller(focusControllerPtr);

      auto keyControllerPtr = Gtk::EventControllerKey::create();
      keyControllerPtr->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType) -> bool
        {
          if (keyval == GDK_KEY_Escape)
          {
            collapse();
            return true;
          }

          return false;
        },
        false);
      _entry.add_controller(keyControllerPtr);

      append(_entry);
    }

    ~AddTagTrigger() override = default;

    AddTagTrigger(AddTagTrigger const&) = delete;
    AddTagTrigger& operator=(AddTagTrigger const&) = delete;
    AddTagTrigger(AddTagTrigger&&) = delete;
    AddTagTrigger& operator=(AddTagTrigger&&) = delete;

    SubmitSignal& signalSubmit() { return _submit; }
    FilterSignal& signalFilterChanged() { return _filterChanged; }

    std::string entryText() const { return _entry.get_text().raw(); }
    bool isEntryOpen() const { return _entry.get_visible(); }

    void openEntry()
    {
      _button.set_visible(false);
      _entry.set_visible(true);
      _entry.grab_focus();
      _dismissController.install(*this,
                                 {this},
                                 [this]
                                 {
                                   if (_entry.get_visible())
                                   {
                                     collapse();
                                   }
                                 });
      _filterChanged.emit(); // re-run the flow filter so current chips drop out of add/search mode
    }

  private:
    void collapse()
    {
      _dismissController.remove();
      _entry.set_text("");
      _entry.set_visible(false);
      _button.set_visible(true);
      _filterChanged.emit();
    }

    void onActivate()
    {
      auto const text = _entry.get_text().raw();

      if (text.empty())
      {
        return;
      }

      _submit.emit(text);
      _entry.set_text(""); // Keep the entry open and focused for rapid successive adds.
      _entry.grab_focus();
    }

    // Clicking away dismisses the inline entry without committing — same as Escape — so the add
    // box never feels stuck open. Enter is the only path that commits a tag.
    void onFocusLeave()
    {
      if (_entry.get_visible())
      {
        collapse();
      }
    }

    static constexpr int kEntryWidthChars = 12;
    static constexpr int kEntryMaxWidthChars = 20;

    Gtk::Button _button;
    Gtk::Entry _entry;
    SubmitSignal _submit;
    FilterSignal _filterChanged;
    DismissController _dismissController;
  };

  TagEditor::TagEditor()
  {
    set_overflow(Gtk::Overflow::HIDDEN);
    setupUi();
  }

  TagEditor::~TagEditor()
  {
    for (auto* child = get_first_child(); child != nullptr;)
    {
      auto* const next = child->get_next_sibling();
      child->unparent();
      child = next;
    }
  }

  Gtk::SizeRequestMode TagEditor::get_request_mode_vfunc() const
  {
    return Gtk::SizeRequestMode::HEIGHT_FOR_WIDTH;
  }

  void TagEditor::measure_vfunc(Gtk::Orientation orientation,
                                int forSize,
                                int& minimum,
                                int& natural,
                                int& minimumBaseline,
                                int& naturalBaseline) const
  {
    minimumBaseline = -1;
    naturalBaseline = -1;

    auto const noPlace = [](Gtk::Widget const*, std::int32_t, std::int32_t, std::int32_t, std::int32_t) {};

    if (orientation == Gtk::Orientation::HORIZONTAL)
    {
      // Stay fully compressible for narrow detail panes; natural is the whole run on a single line.
      minimum = 0;
      natural = layoutFlow(get_first_child(), kUnconstrainedWidth, chipFlowGap(*this), noPlace).naturalWidth;
      return;
    }

    auto const width = forSize < 0 ? kUnconstrainedWidth : forSize;
    auto const height = layoutFlow(get_first_child(), width, chipFlowGap(*this), noPlace).height;
    minimum = forSize < 0 ? 0 : height;
    natural = height;
  }

  void TagEditor::size_allocate_vfunc(int width, int /*height*/, int /*baseline*/)
  {
    layoutFlow(
      get_first_child(),
      width,
      chipFlowGap(*this),
      [](Gtk::Widget* child, std::int32_t xPos, std::int32_t yPos, std::int32_t childWidth, std::int32_t childHeight)
      { child->size_allocate(Gtk::Allocation{xPos, yPos, childWidth, childHeight}, -1); });
  }

  void TagEditor::setup(rt::Library const& reads, std::vector<TrackId> selectedTrackIds)
  {
    _reads = &reads;
    _selectedTrackIds = std::move(selectedTrackIds);

    collectTagData();
    rebuildChips();
  }

  void TagEditor::setupUi()
  {
    add_css_class("ao-tag-editor");

    _addTrigger = Gtk::make_managed<AddTagTrigger>();
    _addTrigger->set_parent(*this);
    _addTrigger->signalSubmit().connect(sigc::mem_fun(*this, &TagEditor::onAddSubmitted));
    _addTrigger->signalFilterChanged().connect([this] { applyFilter(); });
  }

  void TagEditor::insertBeforeTrigger(Gtk::Widget& child)
  {
    // Parents `child` (sinking the floating ref of a freshly make_managed chip) and positions it
    // immediately before the persistent add trigger, keeping the trigger the trailing child.
    child.insert_before(*this, *_addTrigger);
  }

  void TagEditor::collectTagData()
  {
    _currentTags.clear();
    _availableTagsByFrequency.clear();
    _pendingAdds.clear();
    _pendingRemoves.clear();

    if (_reads == nullptr || _selectedTrackIds.empty())
    {
      return;
    }

    auto scope = _reads->reader();

    _currentTags = scope.selectionTags(_selectedTrackIds);

    for (auto const& tag : scope.allTagsByFrequency())
    {
      if (_availableTagsByFrequency.size() >= kMaxAvailableTags)
      {
        break;
      }

      _availableTagsByFrequency.push_back(tag);
    }
  }

  void TagEditor::rebuildChips()
  {
    // Remove every chip, keeping the persistent add trigger as the trailing child so its open
    // state and focus survive across rebuilds.
    auto stale = std::vector<Gtk::Widget*>{};

    for (auto* child = get_first_child(); child != nullptr; child = child->get_next_sibling())
    {
      if (child != static_cast<Gtk::Widget*>(_addTrigger))
      {
        stale.push_back(child);
      }
    }

    for (auto* const child : stale)
    {
      child->unparent();
    }

    auto const addCurrentChip = [this](std::string const& tag)
    {
      auto* const chip = Gtk::make_managed<TagChip>(tag);
      chip->signal_remove().connect([this, tag] { onTagRemoveClicked(tag); });
      insertBeforeTrigger(*chip);
    };

    auto const addAvailableChip = [this](std::string const& tag)
    {
      auto* const chip = Gtk::make_managed<AvailableTagChip>(tag);
      chip->signal_clicked().connect([this, tag] { onAvailableTagClicked(tag); });
      insertBeforeTrigger(*chip);
    };

    for (auto const& tag : _currentTags)
    {
      if (!std::ranges::contains(_pendingRemoves, tag))
      {
        addCurrentChip(tag);
      }
    }

    for (auto const& tag : _pendingAdds)
    {
      if (!std::ranges::contains(_currentTags, tag))
      {
        addCurrentChip(tag);
      }
    }

    for (auto const& tag : _pendingRemoves)
    {
      addAvailableChip(tag);
    }

    for (auto const& [tag, freq] : _availableTagsByFrequency)
    {
      if (std::ranges::contains(_currentTags, tag) || std::ranges::contains(_pendingRemoves, tag) ||
          std::ranges::contains(_pendingAdds, tag))
      {
        continue;
      }

      addAvailableChip(tag);
    }

    applyFilter();
  }

  void TagEditor::onTagRemoveClicked(std::string const& tag)
  {
    if (!std::ranges::contains(_pendingRemoves, tag))
    {
      _pendingRemoves.push_back(tag);
    }

    std::erase(_pendingAdds, tag);

    rebuildChips();

    _tagsChanged.emit(_pendingAdds, _pendingRemoves);
  }

  void TagEditor::onAvailableTagClicked(std::string const& tag)
  {
    if (!std::ranges::contains(_pendingAdds, tag))
    {
      _pendingAdds.push_back(tag);
    }

    std::erase(_pendingRemoves, tag);

    rebuildChips();

    _tagsChanged.emit(_pendingAdds, _pendingRemoves);
  }

  void TagEditor::onAddSubmitted(std::string const& tag)
  {
    if (tag.empty())
    {
      return;
    }

    if (!std::ranges::contains(_pendingAdds, tag))
    {
      _pendingAdds.push_back(tag);
    }

    std::erase(_pendingRemoves, tag);

    rebuildChips();

    _tagsChanged.emit(_pendingAdds, _pendingRemoves);
  }

  void TagEditor::applyFilter()
  {
    auto const open = (_addTrigger != nullptr) && _addTrigger->isEntryOpen();
    auto const search = open ? toLower(_addTrigger->entryText()) : std::string{};

    for (auto* child = get_first_child(); child != nullptr; child = child->get_next_sibling())
    {
      if (child == static_cast<Gtk::Widget*>(_addTrigger))
      {
        continue; // the add trigger is always visible
      }

      if (auto const* const avail = dynamic_cast<AvailableTagChip*>(child); avail != nullptr)
      {
        // Suggested chips live-filter by a case-insensitive substring of the entry text; with no
        // query (entry closed or empty) every suggestion is shown.
        child->set_visible(search.empty() || toLower(avail->getTag()).find(search) != std::string::npos);
      }
      else
      {
        // Current chips (Type A) hide while the inline entry is open so add/search mode shows only
        // the suggestions and the entry, and reappear once it is dismissed.
        child->set_visible(!open);
      }
    }

    queue_resize();
  }
} // namespace ao::gtk
