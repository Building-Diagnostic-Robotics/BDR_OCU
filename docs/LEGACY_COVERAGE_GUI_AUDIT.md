# Legacy monolithic GUI — audit & removal

## Current state (post-cleanup)

- **`CoverageGUI` deleted:** `cpp/include/coverage_gui.hpp` and
  `cpp/src/coverage_gui.cpp` removed from the repo. Nothing instantiated
  it from `main.cpp`; the live shell is `AppShellWindow`.
- **Extracted before deletion:** `PlotWidget` + `ReprojectionLine`
  (`plot_widget.hpp/cpp`), `VideoStreamWidget` (`video_stream_widget.hpp/cpp`),
  `CoverageStats` (`coverage_stats.hpp`), shared polygon helpers
  (`coverage_geometry.hpp`).
- **Recordings / cloud tab / teleop / session tracker:** sources still
  exist (`data_transfer_dialog.*`, `cloud_upload_dialog.*`,
  `scan_session_tracker.*`, `teleop_widget.*`, `network_monitor.*`) but are
  **not** compiled into `bdr_coverage_planner`. Re-add them to `GUI_SOURCES`
  (and matching `HEADERS` for Qt MOC) when `viewRecordingsRequested` is
  wired. **`transfer_manager.cpp`** and **`cloud_upload_manager.cpp`**
  stay linked — `app_shell.cpp` uses both singletons for nav gates.

## Historical notes

Earlier revisions of this file documented include-graph analysis and the
extraction order; that content is obsolete now that `coverage_gui.*` is gone.

---

*Cleanup completed; kept for operator/agent context.*
