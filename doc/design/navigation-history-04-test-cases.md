# Navigation History — Test Coverage Specification

## File Organization

```
test/unit/runtime/NavigationHistoryTest.cpp   — NavigationHistory unit tests
test/unit/runtime/HeadlessShellTest.cpp       — WorkspaceService integration tests (extend existing)
test/unit/runtime/NavigationWorkspaceTest.cpp — WorkspaceService navigation unit tests
test/unit/linux-gtk/NavigationGtkTest.cpp     — GTK mouse + presentation button tests
```

## Shared Test Data

### Lists

| Name | Value | Meaning |
|---|---:|---|
| `L_ALL` | `rt::kAllTracksListId` | Built-in all tracks |
| `L_A` | `ListId{10}` | User list A |
| `L_B` | `ListId{20}` | User list B |
| `L_C` | `ListId{30}` | User list C |

### Presentations

| Name | Source |
|---|---|
| `P_SONGS` | `defaultTrackPresentationSpec()` |
| `P_ALBUMS` | `builtinTrackPresentationPreset("albums")->spec` |
| `P_ARTISTS` | `builtinTrackPresentationPreset("artists")->spec` |
| `P_CUSTOM` | `TrackPresentationSpec{.id = "custom-a", .groupBy = TrackGroupKey::Composer, .sortBy = {{TrackSortField::Year}}, .visibleFields = {TrackField::Title, TrackField::Composer, TrackField::Year}}` |

### Navigation Points (short aliases)

| Name | Value |
|---|---|
| `NP_A` | `{L_A, "", P_SONGS}` |
| `NP_B` | `{L_B, "", P_SONGS}` |
| `NP_C` | `{L_C, "", P_SONGS}` |
| `NP_A_ALBUMS` | `{L_A, "", P_ALBUMS}` |
| `NP_FILTER` | `{L_ALL, "genre == \"Rock\"", P_SONGS}` |

---

## Category A — NavigationHistory Unit Tests

**File:** `test/unit/runtime/NavigationHistoryTest.cpp`

These are pure data-structure tests: no runtime, no views, no services. Only
`NavigationHistory` + `NavigationPoint`.

### A.1 Empty / Construction

| # | Test | Steps | Expected |
|---|------|-------|----------|
| A1.1 | `empty_on_construct` | `NavigationHistory h;` | `size() == 0`, `currentIndex() == nullopt`, `canGoBack() == false`, `canGoForward() == false`, `current() == nullopt`, `back() == nullopt`, `forward() == nullopt` |
| A1.2 | `max_size_clamped_to_one` | `NavigationHistory h{0};` | internal max = 1, construction succeeds |
| A1.3 | `default_max_size` | `NavigationHistory h;` | internal max = 256 (`kDefaultMaxSize`) |

### A.2 Single Commit

| # | Test | Steps | Expected |
|---|------|-------|----------|
| A2.1 | `commit_first_point` | `h.commit(NP_A)` | `size() == 1`, `currentIndex() == 0`, `current() == NP_A`, `canGoBack() == false`, `canGoForward() == false` |
| A2.2 | `commit_twice` | `h.commit(NP_A); h.commit(NP_B)` | `size() == 2`, `currentIndex() == 1`, `current() == NP_B`, `canGoBack() == true`, `canGoForward() == false` |
| A2.3 | `commit_three` | commit A, B, C | `size() == 3`, `currentIndex() == 2`, `current() == NP_C` |

### A.3 Dedup

| # | Test | Steps | Expected |
|---|------|-------|----------|
| A3.1 | `dedup_identical_current` | `h.commit(NP_A); h.commit(NP_A)` | `size() == 1`, `currentIndex() == 0` |
| A3.2 | `dedup_after_back_does_not_dedup` | commit A, B, A, back to B, commit A | size = 3 (A, B, A), currentIndex = 2. The second A matches current (B) only at the dedup check AFTER future truncation — no dedup. |
| A3.3 | `different_presentation_not_deduped` | commit `NP_A`, then `NP_A_ALBUMS` | `size() == 2` |
| A3.4 | `different_filter_not_deduped` | commit `NP_A`, then `NP_FILTER` | `size() == 2` |
| A3.5 | `different_list_not_deduped` | commit `NP_A`, then `NP_B` | `size() == 2` |

### A.4 Back / Forward

| # | Test | Steps | Expected |
|---|------|-------|----------|
| A4.1 | `back_from_two` | commit A, B; `back()` | returns `NP_A`, `currentIndex() == 0`, `canGoBack() == false`, `canGoForward() == true` |
| A4.2 | `back_from_three` | commit A, B, C; `back()` | returns `NP_B`, `currentIndex() == 1`, `canGoBack() == true`, `canGoForward() == true` |
| A4.3 | `back_at_boundary` | commit A; `back()` | returns `nullopt`, `currentIndex() == 0` unchanged |
| A4.4 | `back_on_empty` | `NavigationHistory h; h.back()` | returns `nullopt` |
| A4.5 | `forward_after_back` | commit A, B, C; `back(); forward()` | returns `NP_C`, `currentIndex() == 2`, `canGoForward() == false` |
| A4.6 | `forward_at_boundary` | commit A; `forward()` | returns `nullopt`, index unchanged |
| A4.7 | `forward_on_empty` | `NavigationHistory h; h.forward()` | returns `nullopt` |
| A4.8 | `back_then_forward_mid` | commit A, B, C, D; back (to C); back (to B); forward (to C) | returns C |
| A4.9 | `back_forward_idempotent` | commit A, B, C; back (to B); back (to A); forward (to B); forward (to C) | final at C |

### A.5 Truncation (Browser Model)

| # | Test | Steps | Expected |
|---|------|-------|----------|
| A5.1 | `commit_truncates_future` | commit A, B, C; back to B; commit D | points = [A, B, D], `currentIndex() == 2`, C is gone |
| A5.2 | `truncate_all_future` | commit A, B, C; back to A; commit D | points = [A, D], `currentIndex() == 1` |
| A5.3 | `truncate_from_empty` | `NavigationHistory h; h.commit(NP_A)` | size = 1, no crash |

### A.6 Max Size Eviction

| # | Test | Steps | Expected |
|---|------|-------|----------|
| A6.1 | `evict_from_front` | `NavigationHistory h{3}`; commit A, B, C, D | points = [B, C, D], `currentIndex() == 2`, `back()` returns C |
| A6.2 | `eviction_keeps_index_consistent` | max = 3; commit A, B, C; back to B; commit D | points = [A, B, D], index = 2, no front eviction (size = 3) |
| A6.3 | `max_size_one_eviction` | `NavigationHistory h{1}`; commit A; commit B | points = [B], `currentIndex() == 0`, `canGoBack() == false` |
| A6.4 | `max_size_one_back` | max = 1; commit A; `back()` | returns `nullopt` (only 1 point, at index 0) |

### A.7 Current (Read-Only)

| # | Test | Steps | Expected |
|---|------|-------|----------|
| A7.1 | `current_on_empty` | `NavigationHistory h; h.current()` | returns `nullopt` |
| A7.2 | `current_after_commit` | commit A; `h.current()` | returns `NP_A` |
| A7.3 | `current_after_back` | commit A, B; back; `h.current()` | returns `NP_A` |
| A7.4 | `current_returns_copy` | auto p = h.current(); modify p; h.current() | second current() unchanged |

---

## Category B — WorkspaceService Navigation Tests

**File:** `test/unit/runtime/NavigationWorkspaceTest.cpp`

These use a headless `AppRuntime` (same pattern as `HeadlessShellTest.cpp`).

### B.1 Navigate To

| # | Test | Initial | Steps | Expected |
|---|------|---------|-------|----------|
| B1.1 | `first_navigateTo_commits` | fresh runtime, no views | `navigateTo(L_A)` | active list = A, `canGoBack() == false`, `canGoForward() == false` |
| B1.2 | `second_navigateTo_commits` | A active, history = [A] | `navigateTo(L_B)` | active list = B, `canGoBack() == true`, `canGoForward() == false` |
| B1.3 | `navigateTo_same_list_dedups` | A active | `navigateTo(L_A)` | history size unchanged, no duplicate entry |
| B1.4 | `navigateTo_AllTracks` | A active | `navigateTo(GlobalViewKind::AllTracks)` | active list = AllTracks, one new history point |
| B1.5 | `navigateTo_with_recordHistory_false` | A active | `navigateTo(L_B, {.recordHistory = false})` | active = B, history UNCHANGED, `canGoBack()` as before |
| B1.6 | `navigateTo_query` | A active, no filter | `navigateTo("genre == \"Rock\"")` | active has filter "genre == \"Rock\"", one new point committed |
| B1.7 | `filtered_view_restore_reuses` | A→Rock filter→B→back to Rock→B→back to Rock | inspect | only one Rock-filtered view exists, not duplicates |

### B.2 Go Back / Go Forward

| # | Test | Initial | Steps | Expected |
|---|------|---------|-------|----------|
| B2.1 | `goBack_restores_list` | A → B → C | `goBack()` | active = B, `canGoBack() == true`, `canGoForward() == true` |
| B2.2 | `goBack_twice_restores_first` | A → B → C | `goBack(); goBack()` | active = A, `canGoBack() == false`, `canGoForward() == true` |
| B2.3 | `goForward_after_back` | A → B → C; back | `goForward()` | active = C, `canGoForward() == false` |
| B2.4 | `goBack_at_boundary` | only A committed | `goBack()` | returns `false`, active still A |
| B2.5 | `goForward_at_boundary` | only A committed | `goForward()` | returns `false` |
| B2.6 | `new_navigation_after_back_truncates` | A → B → C; back to B | `navigateTo(L_A)` | active = A, `canGoForward() == false`, C discarded |
| B2.7 | `back_restores_presentation` | A songs → setActivePresentation(P_ALBUMS) → back | check active | active presentation = P_SONGS |
| B2.8 | `back_restores_filter` | A → filter Rock → back | check active | active filterExpression empty |
| B2.9 | `back_restores_custom_presentation` | A → add custom C → setActivePresentation(P_CUSTOM) → navigateTo B → back | check active | active = A, presentation = P_CUSTOM (stored by value) |

### B.3 Deleted List Edge Cases

| # | Test | Initial | Steps | Expected |
|---|------|---------|-------|----------|
| B3.1 | `close_active_then_back` | A → B; closeView(B) | `goBack()` | returns true, active = A |
| B3.2 | `deleted_list_history_point` | A → B; delete list A via mutation | `goBack()` | returns false OR restores to B; must not crash |

### B.4 NavigationHistoryChanged Signal

| # | Test | Steps | Expected |
|---|------|-------|----------|
| B4.1 | `signal_emits_on_navigate` | subscribe; `navigateTo(L_B)` | handler called with `{canGoBack: true, canGoForward: false}` |
| B4.2 | `signal_emits_on_back` | A → B; subscribe; `goBack()` | handler called with `{canGoBack: false, canGoForward: true}` |
| B4.3 | `signal_not_emitted_on_dedup` | subscribe; `navigateTo(L_A)` twice | handler called once |
| B4.4 | `signal_not_emitted_on_noop` | A active; `setActivePresentation(P_SONGS)` when already songs | handler not called |

### B.5 Initial Commit (Session Restore)

| # | Test | Steps | Expected |
|---|------|-------|----------|
| B5.1 | `session_restore_commits_initial` | save session A; new runtime; `restoreSession()` | active = A, history size = 1, `canGoBack() == false` |
| B5.2 | `session_restore_empty_no_commit` | no saved session; new runtime; `restoreSession()` | history size = 0 |
| B5.3 | `restore_then_navigate_back` | restore A; `navigateTo(L_B)`; `goBack()` | active = A (restored view), `canGoBack() == false` |
| B5.4 | `restore_not_recorded_as_duplicate` | restore A; `navigateTo(L_B)`; `goBack()` → user at A; `navigateTo(L_B)` again; `goBack()` | active = A (not B again) |

### B.6 Replay Guard

| # | Test | Steps | Expected |
|---|------|-------|----------|
| B6.1 | `back_does_not_commit` | A → B; `goBack()` | history size = 2 (not 3), `canGoForward() == true` |
| B6.2 | `forward_does_not_commit` | A → B; `goBack()`; `goForward()` | history size = 2 (not 3) |
| B6.3 | `back_forward_noop_does_not_grow` | A → B; `goBack()`; `goForward()` | size = 2 throughout |

---

## Category C — WorkspaceService Presentation & Compound Commands

### C.1 setActivePresentation (TrackPresentationSpec)

| # | Test | Initial | Steps | Expected |
|---|------|---------|-------|----------|
| C1.1 | `setActivePresentation_commits` | A songs | `setActivePresentation(P_ALBUMS)` | active = albums, history grows by 1, `canGoBack() == true` |
| C1.2 | `back_restores_previous_presentation` | A songs; `setActivePresentation(P_ALBUMS)` | `goBack()` | active = songs |
| C1.3 | `no_active_view_safe` | fresh runtime, no views | `setActivePresentation(P_ALBUMS)` | no crash, no history entry |
| C1.4 | `dedup_same_spec` | A albums committed | `setActivePresentation(P_ALBUMS)` | history unchanged |
| C1.5 | `recordHistory_false` | A songs | `setActivePresentation(P_ALBUMS, {.recordHistory = false})` | active = albums, history unchanged |

### C.2 setActivePresentation (string_view)

| # | Test | Steps | Expected |
|---|------|-------|----------|
| C2.1 | `builtin_by_id` | A songs; `setActivePresentation("albums")` | active = albums preset |
| C2.2 | `custom_by_id` | add custom; `setActivePresentation("custom-a")` | active = custom spec |
| C2.3 | `unknown_id_returns_empty` | `setActivePresentation("nonexistent")` | returns `{}`, active unchanged, no history entry |

### C.3 jumpToAlbum

| # | Test | Initial | Steps | Expected |
|---|------|---------|-------|----------|
| C3.1 | `commits_once` | A songs, playing `T_BLUE_1` | `jumpToAlbum(T_BLUE_1)` | history grows by 1 only; one `goBack()` returns to A songs |
| C3.2 | `final_state_correct` | A songs | `jumpToAlbum(T_BLUE_1)` | active list = AllTracks, presentation = albums |
| C3.3 | `invalid_track_noop` | current track = `kInvalidTrackId` | `jumpToAlbum(kInvalidTrackId)` | no history entry, no crash, no state change |
| C3.4 | `back_restores_pre_jump_view` | A songs | jump album; `goBack()` | active = A, songs presentation |

---

## Category D — ViewService Cleanup Verification

**File:** `test/unit/runtime/ViewServiceTest.cpp` (modify existing)

| # | Test | Steps | Expected |
|---|------|-------|----------|
| D1 | `setPresentation_updates_state_and_projection` | create view; `setPresentation(P_ALBUMS)` | state.groupBy = Album, state.presentation.id = "albums" |
| D2 | `setPresentation_noop_same_normalized` | set P_ALBUMS twice | one presentation event at most |
| D3 | `createView_applies_initial_presentation` | create with config having groupBy | state.groupBy matches, sort derived correctly |
| D4 | `legacy_symbols_not_compilable` | attempt `views.setGrouping(...)` | compile error (verification via build) |

---

## Category E — GTK Integration Tests

**File:** `test/unit/linux-gtk/NavigationGtkTest.cpp`

These require a GTK test environment (existing pattern in `ao_test_gtk` target).

### E.1 Mouse Buttons

| # | Test | User Action | Expected |
|---|------|-------------|----------|
| E1.1 | `mouse_back_navigates` | click sidebar A, click sidebar B, press button 8 | active = A |
| E1.2 | `mouse_forward_after_back` | after E1.1, press button 9 | active = B |
| E1.3 | `mouse_back_at_boundary_safe` | only one history point, press button 8 | no state change, no crash |
| E1.4 | `mouse_forward_at_boundary_safe` | only one point, press button 9 | no state change, no crash |

### E.2 Presentation Button

| # | Test | User Action | Expected |
|---|------|-------------|----------|
| E2.1 | `presentation_button_records` | navigate to A songs; select "Albums" in presentation dropdown; press back | active = A, songs |
| E2.2 | `presentation_button_idle_uses_active_view` | select Albums; immediately switch to B list before idle fires | presentation applied to B (the current active), not the view from click time |

### E.3 Compound Actions

| # | Test | User Action | Expected |
|---|------|-------------|----------|
| E3.1 | `jump_to_album_single_back` | A songs; click cover art (jumpToAlbum action); press button 8 | one press → A songs |
| E3.2 | `filter_by_field_label_records` | A all tracks; click artist label (filterByField); press back | active = all tracks, no filter |

---

## Build & Run

```bash
# Unit tests (no GTK)
nix-shell --run "/tmp/build/debug/test/ao_test \"[navigation]\""

# GTK integration tests
nix-shell --run "/tmp/build/debug/test/ao_test_gtk \"[navigation]\""

# All tests
nix-shell --run "/tmp/build/debug/test/ao_test"
```

Test tags:
- `[navigation]` — all navigation tests
- `[navigation][unit]` — NavigationHistory data structure
- `[navigation][workspace]` — WorkspaceService navigation
- `[navigation][gtk]` — GTK integration
