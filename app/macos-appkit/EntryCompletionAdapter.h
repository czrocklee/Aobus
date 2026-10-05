// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/i18n/MessageCatalog.h>
#include <ao/rt/completion/CompletionItem.h>
#include <ao/rt/completion/CompletionResult.h>

#import <AppKit/AppKit.h>

#include <cstddef>
#include <string>
#include <vector>

namespace ao::appkit
{
  struct EntryCompletionOptions final
  {
    bool acceptOnReturn = false;
  };

  // The host forwards text changes, commands, and focus end. Detach before
  // the borrowed field or provider's vocabulary retires; delegates stay host-owned.
  class EntryCompletionAdapter final
  {
  public:
    EntryCompletionAdapter(NSTextField* field,
                           i18n::MessageCatalog textCatalog,
                           rt::CompletionProvider provider,
                           EntryCompletionOptions options = {});
    ~EntryCompletionAdapter();
    EntryCompletionAdapter(EntryCompletionAdapter const&) = delete;
    EntryCompletionAdapter& operator=(EntryCompletionAdapter const&) = delete;
    EntryCompletionAdapter(EntryCompletionAdapter&&) = delete;
    EntryCompletionAdapter& operator=(EntryCompletionAdapter&&) = delete;

    void update();
    void dismiss();
    // Requires unchanged source/caret and reports whether it inserted; settles
    // before native insertion because synchronous host callbacks can retire this adapter.
    bool tryApplySelected();
    bool tryHandleCommand(SEL command);
    // Forwarded by the adapter's own field-editor observer while the popover is shown.
    void handleEditorSelectionChange();
    void detach();

  private:
    // Native views are built on first show: most completable fields never open one.
    void ensureNativeViews();
    NSTextView* activeEditor() const;
    void clearCompletionState();
    bool tryMoveSelection(std::ptrdiff_t delta);
    bool tryMovePageSelection(std::ptrdiff_t direction);

    __weak NSTextField* _field;
    i18n::MessageCatalog _textCatalog;
    rt::CompletionProvider _provider;
    EntryCompletionOptions _options;
    NSPopover* _popover = nil;
    NSScrollView* _scrollView = nil;
    NSTableView* _tableView = nil;
    id<NSTableViewDataSource, NSTableViewDelegate, NSPopoverDelegate> _tableAdapter = nil;
    std::vector<rt::CompletionItem> _items;
    std::string _sourceText;
    std::size_t _replaceBegin = 0;
    std::size_t _replaceEnd = 0;
    NSRange _replacementRange = NSMakeRange(0, 0);
    NSUInteger _sourceCursorUtf16 = 0;
    // Suppress acceptance's own text notification until the input differs.
    std::string _suppressedText;
    bool _hasSuppression = false;
    bool _hasReplacement = false;
    bool _detached = false;
  };
} // namespace ao::appkit
