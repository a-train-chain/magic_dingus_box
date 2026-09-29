# Final fix report
1. CLAUDE.md: "Re-downloading a deleted season" rewritten (suggested_season + chooser, season-list SELECT, N>1 add with monitor none, set_season_monitored flips series flag); Library keeps empty shows sentence added near "Library grid".
2. Stale comments fixed: series_detail_screen.h (intent), .cpp (drain intent comment, whole-series add comment), series_detail_logic.h (2 places), test_series_detail_logic.cpp (2 places).
3. test_series_detail_logic.cpp: deleted-season test now uses suggested_season(rows, watch) (season_choice.h included), asserts label "Download Season 2" and season 3 in eligible_seasons.
4. Spec "Error handling" and test_season_choice.cpp comments reworded to "next at or above (else last)".
5. Leave-mid-add toast is title-prefixed via new mut_start_title_ (guarded by mut_mtx_, published with mut_start_season_).
6. test_sonarr_client.cpp: REQUIRE(at != npos) after the fixture find.
7. start_season_download: early mut_in_flight_ guard toast "Still finishing the last action..." + return.
8. Plan Task 7: Step 4b added; Step 2/4 python prints series-level monitored.
## Verification
- test_media_browser_unit: "All tests passed (7378 assertions in 479 test cases)"
- pisim check: "pisim: kiosk binary OK (aarch64, sd_notify, Media Browser, GPIO)", "100% tests passed, 0 tests failed out of 9", "367 passed, 2 skipped"
