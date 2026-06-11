# BDR Operator Control Unit (OCU)

**BDR Coverage Planner** — the desktop Operator Control Unit for *Roofus*,
Building Diagnostic Robotics' autonomous mobile robot for building roof
scanning. The OCU is the single application an operator uses to bring up a
robot, drive an exploration pass, plan coverage, run an autonomous scan,
and ship the resulting data to the cloud.

> Internal repository — distribute only inside Building Diagnostic Robotics
> and approved field-test partners.

---

## At a glance

| | |
|---|---|
| **Repository** | `Building-Diagnostic-Robotics/BDR_OCU` |
| **Binary** | `bdr_coverage_planner` (`.deb`, x86-64, Ubuntu 22.04) |
| **Languages** | C++17 (Qt Widgets), CMake, GitHub Actions |
| **Runtime** | ROS 2 Humble, CycloneDDS, Zenoh (`zenoh-ros2dds` bridge) |
| **License** | MIT — see [`LICENSE`](LICENSE) |
| **Release channel** | GitHub Releases, rolling tag `latest` (per push to `main`) |
| **Updates** | In-app OTA with auto-rollback (see [`docs/OTA.md`](docs/OTA.md)) |

---

## Key features

### Operator workflow

- **Staged UI** — Setup → Pre-flight → Dashboard → Exploration → Coverage
  Planning. Each stage is a self-contained Qt screen owned by `AppShellWindow`.
- **Pre-flight check & login** — verifies robot reachability and authenticates
  the operator over SSH against `pilot_control_auth` before any autonomy
  command is allowed.
- **Dashboard rollup** — live System Status (`INITIALIZING / READY / WARNING
  / NOT READY`), battery and signal pills, total scans, "next calibration"
  reminder with click-to-launch tilt calibration, system uptime, and quick
  actions for **Start New Scan** and **Upload Data**.
- **Exploration screen** — teleop, FPV video, live odometry, mapping status,
  thermal/visual stream switching, and one-touch *Save Map*.
- **Coverage planner** — load a saved map, draw boundary / ROI / manual
  obstacles, run automatic obstacle detection from the point cloud, generate
  swaths and a coverage path via Fields2Cover, publish waypoints, and drive
  an autonomous scan with per-segment progress, quality and live FPV.

### Coverage pipeline

- **Boundary extraction** — alpha-shape concave hull via CGAL (convex-hull
  fallback when CGAL is absent).
- **Automatic obstacle detection** — RANSAC ground plane → above-/below-band
  candidates → PCL statistical outlier removal → DBSCAN clustering →
  union-find merge → grid contouring with hole preservation and rolling-disk
  smoothing. Pure C++ port of the legacy Python reference.
- **Swath + path planning** — Fields2Cover-driven; presets persisted via
  `PresetManager` / `QSettings`.

### Mission lifecycle

- **New-scan metadata modal** — captures Building name, Operator name, and a
  Metric / ANSI units toggle. Persisted to `QSettings` and pushed to the
  robot's `data_collection_coordinator` via
  `rcl_interfaces/srv/SetParameters` **before** autonomy is armed. A failed
  push hard-blocks the scan from starting.
- **Per-building data layout** — robot writes
  `/R_DATA/<Month_DD_YYYY>/<building_slug>/Section_<N>_<HHMMSS>/` with sibling
  `Mission_<HHMMSS>/mission_config.json`. The OCU's dashboard probe and
  upload tooling expect this depth-3 layout.
- **Units system (display-only)** — `UnitsProvider` singleton +
  `units::format{Length,Speed,Area}` helpers. All ROS payloads, internal
  state, and on-disk scan data remain SI; only display strings change.

### Connectivity & resilience

- **Layered link health** — combines (1) ROS topic freshness via
  `LinkHealthMonitor`, (2) ICMP/TCP-22 reachability via
  `RobotReachabilityProbe`, and (3) FPV proof-of-life from the live RTP/UDP
  video stream. Resolves to `Healthy / Reconnecting / Disconnected` and is
  surfaced as a top-bar **BOT** pill, an inline disconnect banner, and an
  amber map halo.
- **E-Stop bypass** — Emergency Stop stays enabled while the link is merely
  *Reconnecting*; only fully disabled when the link is provably offline.
- **Offline finalize** — if the operator hits *Complete Mission* with the
  robot unreachable, an `OfflineFinalizeDialog` offers **Wait for
  reconnect** (default, 5 min auto-fallback), **Finalize via SSH (offline)**,
  or **Cancel**. The offline path runs `finalize_mission_local.py` directly
  via `python3` and tears down the launch tree.
- **Robot-side safety nets** — atomic JSON writes, 1 Hz mission heartbeat,
  10 min idle auto-finalize watchdog with `finalized_via` provenance.
- **Seek thermal USB recovery** — in-process drop-rate and connect
  watchdogs trigger a programmatic USB re-enumeration (`seek_usb_reset.py` +
  udev rule) without requiring an operator unplug.

### Cloud upload

- **Robot → S3 direct** via short-lived presigned URLs minted by the BDR
  backend API. The laptop holds **zero** AWS credentials.
- Per-section atomic `run_id = <date>/<building>/<section>`; sentinel files
  on the robot (`upload_state.json`, `manifest.json`) make resume,
  pause, and re-derivation of state idempotent and laptop-reimage-safe.
- Driven from the Dashboard *Upload Data* card via `UploadDialog` /
  `UploadRunner` / `UploadStateProbe` over SSH; hard-blocked while a scan
  is alive.

### OTA update pipeline

- `UpdateChecker` polls GitHub Releases for the rolling `latest` tag, shows
  a non-intrusive banner, and on confirm hands off to an external
  `bdr-update-runner` that downloads, SHA-256-verifies, and `dpkg -i`'s the
  new `.deb` via a NOPASSWD `bdr-apply-update` wrapper.
- 60 s `bootHealthy` watchdog in `main()` automatically **rolls back** to
  the previous `.deb` if the new build fails to come up cleanly.
- Pre-install gating on battery state and active mission; snooze 4 h.
  Persistent rollback denylist prevents repeat-offering a known-bad SHA.

See [`docs/OTA.md`](docs/OTA.md) for the full state machine and
field-test recipe.

---

## Architecture

```
┌────────────────────── AppShellWindow (single QMainWindow) ──────────────────────┐
│                                                                                  │
│  QStackedWidget                                                                  │
│  ├── 1. SetupScreen          (robot pick, settings, login bypass-aware)          │
│  ├── 2. StartupScreen        (pre-flight, SSH auth, launch dashboard)            │
│  ├── 3. DashboardScreen      (status rollup, quick actions, calibration)         │
│  ├── 4. ExplorationScreen    (teleop, FPV, mapping, save map)                    │
│  └── 5. PlannerScreen        (coverage plan + autonomous scan execution)         │
│                                                                                  │
│  Owns:                                                                           │
│  • ROS 2 node (background spinner)                                               │
│  • LinkHealthMonitor + RobotReachabilityProbe                                    │
│  • UnitsProvider, RobotRegistry, PresetManager                                   │
│  • UpdateChecker, UpdateBanner, UpdateModal, RollbackBanner                      │
└──────────────────────────────────────────────────────────────────────────────────┘
        │                                          │
        ▼                                          ▼
  Zenoh ↔ CycloneDDS bridge                  GitHub Releases (HTTPS)
  to robot's ROS 2 graph                     → bdr-update-runner (external)
```

Build dependencies: **Qt 5 or Qt 6** (Core / Widgets / Concurrent / Network /
Svg), **PCL 1.10+** (common / io / filters), **Eigen 3**, **GStreamer 1.0**
(video), **Fields2Cover** (coverage planning), **CGAL** (alpha-shape hulls;
optional). The build prints which optional features are active at configure
time.

---

## Requirements

| | |
|---|---|
| OS | Ubuntu 22.04 LTS (x86-64) |
| ROS | ROS 2 Humble Hawksbill (sourced `pilot_ws` workspace) |
| RMW | `rmw_cyclonedds_cpp` with loopback config at `~/cyclone_loopback.xml` |
| Privileges | Operator user in the `sudo` group (required for OTA install) |
| Network | Reachable robot host (see `cpp/config/robots.json`) |

---

## Installation

### Recommended — install the published `.deb`

Every push to `main` publishes a fresh package to the rolling `latest`
tag. CI prunes old assets, so `latest` holds exactly one `.deb` + its
`.sha256` sidecar.

```bash
cd /tmp
gh release download latest \
  -R Building-Diagnostic-Robotics/BDR_OCU \
  --pattern 'bdr-coverage-planner_*_amd64.deb*' --clobber

# Verify integrity, then install (apt resolves runtime deps in one step).
sha256sum -c bdr-coverage-planner_*_amd64.deb.sha256
sudo apt install -y ./bdr-coverage-planner_*_amd64.deb
```

After the first install, **in-app OTA takes over** — every subsequent
release surfaces as a banner on the Dashboard.

If `gh` is unavailable, resolve the single `latest` `.deb` URL via the API:

```bash
cd /tmp
DEB_URL=$(curl -sH "Authorization: token <PAT>" \
  https://api.github.com/repos/Building-Diagnostic-Robotics/BDR_OCU/releases/tags/latest \
  | grep '"browser_download_url".*_amd64\.deb"' | cut -d'"' -f4)

curl -L -H "Authorization: token <PAT>" -o bdr-coverage-planner_amd64.deb "$DEB_URL"
sudo apt install -y ./bdr-coverage-planner_amd64.deb
```

### Build from source

```bash
cd cpp
./build.sh                  # Release (default)
./build.sh Debug            # Debug build
./build/bdr_coverage_planner
```

Or manually:

```bash
mkdir -p cpp/build && cd cpp/build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j"$(nproc)"
```

To produce a local `.deb` (postinst installs the sudoers drop-in via
`visudo -c`):

```bash
cd cpp
./build.sh && ./create_deb.sh
sudo dpkg -i build/bdr-coverage-planner_*_amd64.deb
```

---

## Configuration

### `cpp/config/robots.json`

Fleet registry consumed by `RobotRegistry` at startup. One entry per
robot — `robot_id`, SSH user/host, radio IP, SNMP RSSI/SNR OIDs, and the
backend `cloud_client_id` / `cloud_device_token` used by the upload
pipeline. Robot IPs are **never** shown in the UI.

The top-level `cloud_api_base` (or sibling `cloud_config.json` override)
points the OCU at the backend API Gateway that mints presigned upload
URLs.

### Settings

User preferences are persisted via `QSettings` under organization
`PilotControl`, application `BDRCoveragePlanner`. Keys are centralized in
`cpp/include/settings_constants.hpp`; do not hardcode org/app strings.

### Environment

- `CYCLONEDDS_URI` is cleared at startup if it references `rf_cyclonedds.xml`
  (which would break the `zenoh-ros2dds` discovery path).
- `RMW_IMPLEMENTATION=rmw_cyclonedds_cpp` is required when running outside
  the `.deb` launcher.

---

## Repository layout

```
BDR_OCU/
├── cpp/                  # OCU source — Qt Widgets + ROS 2
│   ├── src/              # Stages, components, OTA runner, upload helpers
│   ├── include/          # Public headers (incl. components/, update/)
│   ├── config/           # robots.json fleet registry
│   ├── cmake/            # Version stamping (git SHA → version_info.hpp)
│   ├── scripts/          # bdr-apply-update wrapper + sudoers drop-in
│   ├── CMakeLists.txt    # Single build for OCU + bdr-update-runner
│   └── create_deb.sh     # Local .deb packaging
├── docs/                 # Internal design docs
│   ├── OTA.md            # OTA state machine + field-test recipe
│   ├── DEV_BYPASSES.md   # Active dev short-circuits + re-wire checklist
│   ├── TILT_CALIBRATION_PLAN.md
│   ├── LEGACY_COVERAGE_GUI_AUDIT.md
│   └── instructions.md
├── vendor/               # Vendored ROS interface packages (e.g. odrive_can)
├── App resource/         # Design assets / exported SVGs
├── screenshots/          # UI screenshots for issue tracking
├── .github/workflows/    # CI: build + .deb + GitHub Release
├── AGENTS.md             # Authoritative agent/contributor context
└── CHANGELOG.md          # Operator-facing release notes (top entry → OTA modal)
```

---

## Documentation

| Document | Purpose |
|---|---|
| [`AGENTS.md`](AGENTS.md) | Authoritative architecture + invariants. **Read before changing the OCU.** |
| [`cpp/CLAUDE.md`](cpp/CLAUDE.md) | Build, namespaces, ROS-thread safety, component map. |
| [`docs/OTA.md`](docs/OTA.md) | OTA state machine, runner UX, rollback, field-test recipe. |
| [`docs/DEV_BYPASSES.md`](docs/DEV_BYPASSES.md) | Intentional dev short-circuits and the checklist to re-wire them before release. |
| [`docs/TILT_CALIBRATION_PLAN.md`](docs/TILT_CALIBRATION_PLAN.md) | Tilt calibration design, dialog flow, TODOs. |
| [`CHANGELOG.md`](CHANGELOG.md) | Per-release notes. Top section ships in the in-app OTA modal. |

---

## Releases & versioning

- **Semver** in `cpp/CMakeLists.txt` (`project(... VERSION x.y.z ...)`).
  Stamped into `version_info.hpp` together with the current git short SHA
  on every build.
- **CI** (`.github/workflows/release.yml`) builds on every push to `main`,
  publishes the `.deb` + `.sha256` sidecar to the rolling `latest` tag and
  to an immutable `v-<short-sha>` tag for rollback.
- Doc-only commits (`README.md`, `AGENTS.md`, `docs/**`, `.cursor/**`) are
  `paths-ignore`'d and do **not** cut a release.

To ship release notes in the next OTA modal, prepend a new `##` block to
[`CHANGELOG.md`](CHANGELOG.md) before merging to `main` — ≤5 bullets,
≤80 chars each, lines must start with `-`.

---

## Development

### Critical invariants

These are load-bearing — do not "clean them up" without an explicit ask.
See [`AGENTS.md`](AGENTS.md) for the full list.

- **No QML, no `.ui` files.** All UI is programmatic C++.
- **Thread safety.** ROS 2 callbacks and `QProcess` handlers must marshal
  to the UI thread via `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`.
- **Frameless windows.** Every dialog uses
  `Qt::Dialog | Qt::FramelessWindowHint` — no native OS chrome.
- **Theming.** Use `uiThemeTokens(bool dark_mode)` from
  `ui_theme_constants.hpp`. Never hardcode colors.
- **Units.** Format display strings exclusively through
  `units::format{Length,Speed,Area}`. ROS payloads stay SI.
- **Dev bypasses.** Tag any new temporary short-circuit with
  `// BDR_REWIRE:` and add an entry to [`docs/DEV_BYPASSES.md`](docs/DEV_BYPASSES.md).

### Before any "prepare for release" task

```bash
rg -n 'BDR_REWIRE|Dev bypass|dev bypass|kEnable.*Passthrough' cpp/
```

Surface each remaining match so the reviewer can decide whether it should
ship.

---

## License

Released under the [MIT License](LICENSE). Copyright © 2026
Building Diagnostic Robotics.
