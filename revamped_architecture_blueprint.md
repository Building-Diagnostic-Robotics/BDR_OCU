# Revamped Architecture Blueprint (Staged Frontend)

## New UI Paradigm & Routing
- **Staged flow router:** The new environment uses a single `QMainWindow` (`AppShellWindow`) with a `QStackedWidget` to host discrete stages (login → diagnostics → dashboard). Stage transitions are driven by Qt signals and slots, forming an explicit multi-step routing pattern.
- **Stepwise, task-focused screens:** Each stage is a self-contained `QWidget` with its own layout and styling, supporting a guided, linear progression rather than a sprawling, all-in-one planner UI.
- **Layout strategy:** UI is built with nested `QVBoxLayout`/`QHBoxLayout`, card-like sections, and `QScrollArea` for long content. Styling relies on per-screen Qt stylesheets (QSS) and property-based theming to keep layouts modular and visually consistent.
- **Frameless shell controls:** The window chrome is custom (frameless with custom minimize/maximize/close controls) and uses an `eventFilter` for window dragging, indicating a branded “app shell” experience.

## Current Implementation Status

### Stage 1: Setup/Login
- **Implemented:** A full login panel with Robot ID + Access Code, connection status indicator, and view/hide access code toggle.
- **Local state:** UI state (validation, error messaging, connection icon) is held within `SetupScreen` members; connection status is polled via `QProcess` ping on a timer.
- **Behavioral status:** Login is currently a dev bypass—credentials are validated for non-empty input and then the stage proceeds. Access code is intentionally not persisted. Robot ID is persisted via `QSettings`.

### Stage 2: Startup/Diagnostics
- **Implemented:** A detailed diagnostics screen with checklist, live results panel, and a log output area.
- **Local state:** `StartupScreen` tracks run state (`preflight_running_`, `preflight_completed_`, `overall_status_`), log lines, and UI badges.
- **Behavioral status:** Runs preflight diagnostics on the robot via SSH (`ros2 run pilot_control startup_preflight`) and fetches a JSON report to populate results. The “Continue” action is currently always enabled (gating logic exists but is bypassed).

### Stage 3: Dashboard
- **Implemented:** A dashboard with status cards, quick actions, and system info fields.
- **Local state:** Primarily presentation state (labels, card text) with some `setRobotId`/`setDarkMode` hooks.
- **Behavioral status:** Emits signals for actions (start scan, run diagnostics, view recordings, logout) but only logout/run-diagnostics are wired in the stage router. The rest are placeholders for future integration.

### Shell & Cross-Cutting Pieces
- **Theme management:** Dark/light mode is persisted in `QSettings` and pushed into each stage via `setDarkMode`.
- **Robot identity:** `AppShellWindow` retains `robot_id_` and `access_code_` in-memory and passes the ID to downstream stages.
- **Legacy planner:** The legacy `CoverageGUI` window is still compiled but not launched by the new staged flow.

## State Management Intent
- **Local ownership per stage:** Each stage encapsulates its UI state with member fields, avoiding a centralized store. This keeps stages independent and simplifies staged transitions.
- **Minimal shared state:** The app shell holds only the user’s robot identifier, access code, and theme preference, then passes the robot ID into downstream screens.
- **Persistent settings:** `QSettings` is used for app-level preferences (org/app constants, dark mode) and limited identifiers (Robot ID).
- **Signal-driven progression:** Navigation and UI updates use Qt signals/slots, emphasizing event-driven state transitions over a global state container.

## Integration Strategy (Preparing for Backend/Data)
- **Robot connectivity scaffolding:** The staged flow already exercises robot connectivity via SSH-based diagnostics and ping checks, establishing a network/robot health gate before deeper workflows.
- **Authentication hooks in place:** `robot_login` and `robot_registry` exist for SSH authentication and robot host resolution, but are not yet wired into `SetupScreen` (currently bypassed). This is a clear insertion point for real auth and robot selection logic.
- **Diagnostics to data pipeline:** The preflight report fetch and JSON parsing in `StartupScreen` demonstrate how robot-side processes can feed structured data into the UI.
- **Routing hooks for legacy features:** Dashboard signals (`startNewScanRequested`, `viewRecordingsRequested`) are defined but not connected—indicating intended expansion points for launching planning workflows, data transfer, or a new web-based frontend.
- **Separation path:** The new flow isolates onboarding and diagnostics from the planner itself, suggesting a future handoff into a planner module or service layer rather than embedding planning logic directly in early stages.

## Key Files (New Environment)
- Stage router: `cpp/src/app_shell.cpp`, `cpp/include/app_shell.hpp`
- Stage 1: `cpp/src/setup_screen.cpp`, `cpp/include/setup_screen.hpp`
- Stage 2: `cpp/src/startup_screen.cpp`, `cpp/include/startup_screen.hpp`
- Stage 3: `cpp/src/dashboard_screen.cpp`, `cpp/include/dashboard_screen.hpp`
- Shared settings: `cpp/include/settings_constants.hpp`
- Planned auth/registry integration: `cpp/src/robot_login.cpp`, `cpp/src/robot_registry.cpp`
