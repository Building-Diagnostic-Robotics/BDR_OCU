# Robot workspace sync

Laptop ↔ robot `~/pilot_ws` updater. Operator-driven. The OCU binary
owns the orchestration so a branch switch cannot delete the code that
performs it. Robot helpers live outside git at `~/pilot_deploy/`.

Read `AGENTS.md` first for the short-form rules.

## What it does

After Dashboard login (`goToStage3`), the OCU waits 30 s, then checks
every 5 minutes:

1. Laptop `~/pilot_ws` HEAD vs compiled-in `kRobotDeployBranch`
   (`cliff-on-autonomy`) and `origin/<deploy>`.
2. Robot `~/pilot_ws` branch/SHA, helpers, and
   `receive.denyCurrentBranch=updateInstead`.

Network or SSH failure on that timer is silent. A laptop that is
already current with the robot merely offline does **not** raise a
banner (`robot_pending`).

Because both of those cases are invisible on screen, **every check
outcome is logged** — one line per finish, tagged `reposync`, in
`<CacheLocation>/update.log` (`~/.cache/PilotControl/BDR Coverage
Planner/update.log`). The line carries the fields the banner decision
reads (`offer=`, `pending=`, laptop and robot branch@sha, `repo_ok=`,
`helpers_ok=`, `recv_ok=`, dirty flags) plus the headline, and rides
the log level (WARN/ERROR for an actionable or failed check). When a
banner did not appear, that log is what distinguishes "robot really is
in sync" from "the SSH probe failed, so the snapshot fell back to
`robot_pending`".

An actionable mismatch raises **Robot software** below the OTA banner
(OTA stays first). View Details opens a frameless modal (`show()`, not
`exec()`): **Sync now** / **Later** (4 h snooze). **Prepare robot**
appears only when Check says helpers or `updateInstead` are missing.
A helper without `--no-build` is treated as missing (same Prepare CTA).

## Sync now

Targets `kRobotDeployBranch` on both sides. If the laptop is on
another branch and the tree is clean, Sync **switches** (never
`--force`). Then:

- `git fetch` / `merge --ff-only` when origin is ahead
- Diff the pre-switch SHA (`switchFromHead_`) to HEAD; rebuild
  `--packages-above` the mapped packages (same path as a same-branch
  sync — a switch is not a full workspace build)
- `git push` (explicit refspec, no `--force`) to the robot
- `~/pilot_deploy/robot_switch_branch.sh --no-build` if the robot
  branch differs, then `rebuild_affected.sh <pre-switch> <new>`
- verify robot HEAD == laptop HEAD

A full `colcon build --symlink-install` is the **fallback**, not
the default. Each side decides from **its own** range: the laptop
from `switchFromHead_` / `laptopHeadBefore_` (unmapped compile
unit or `package.xml` A/D/R/C → `fullBuild_`); the robot from
`rebuild_affected.sh <robotHead_> <new>` — same two triggers.
The OCU always sends `--no-build`; it does not second-guess the
robot's range.

Dirty or diverged trees are hard fails. Nothing is discarded.

`--symlink-install` does not prune `install/`. A *file* removed from
a surviving package can leave a stale install entry; the only real
fix is `rm -rf build install && colcon build` by hand, which this
path will not automate. A package add/remove is what the
`package.xml` guard is for.

The `--no-build` flag has to be on the robot *before* the first
switch. Check greps `~/pilot_deploy/robot_switch_branch.sh` for
`--no-build`; a pre-flag copy reports `helpers=missing` so Prepare
is offered. Re-run **Prepare robot** from a laptop whose
`scripts/deploy/robot_switch_branch.sh` already has the flag
(cherry-picked onto the laptop's current branch if that is not yet
`cliff-on-autonomy`).

## Gates

- `launch_active` composite (same as Upload / closeEvent): Stage 4/5
  launch flags or Stage 6 `missionActive()`. Banner is hidden for the
  mission; Sync/Prepare stay disabled.
- Laptop battery < 20% (`UpdateModal::readBatteryPercent()`).

## Paths

| Side   | Repo                         | Helpers                         |
|--------|------------------------------|---------------------------------|
| Laptop | `~/pilot_ws`                 | `src/pilot_control/scripts/deploy/install_deploy_helpers.sh` |
| Robot  | `~/pilot_ws`                 | `~/pilot_deploy/*.sh` (not in git) |

No auto-clone. Missing `~/pilot_ws` → "Not a git workspace".

## Safety (load-bearing)

The manager never runs `reset --hard`, `checkout -f`, `clean`, or
`push --force`. It does **not** systemd-restart `rdata-offload`; a
successful sync mentions that in the result detail only.

`packagesForChangedFiles` is a static prefix map (lock-step with
`rebuild_affected.sh`, including the unmapped / `package.xml`
guard). It must not call `workspacePackageNames()` — that walks
the laptop tree and is not unit-testable in isolation. The
instance `affectedPackages()` filters the static result to
packages that actually exist. `rebuild_affected.sh` is the
robot's only scoped-vs-full decider.

## Prepare robot

One-time: copies helpers to `~/pilot_deploy` and sets
`receive.denyCurrentBranch=updateInstead` so a push into the checked-out
branch updates the worktree instead of refusing or leaving it stale.
Robot-side scripts already live on `cliff-on-autonomy`; do not rewrite
them from this repo.

## Pin / target

- Deploy branch is compiled in (`kRobotDeployBranch`). Changing it
  ships in the next OCU release. No production branch selector.
- OTA fleet targeting (`ota_targets.json`) is independent — this path
  syncs `~/pilot_ws`, not the OCU `.deb`.

## File index

```
cpp/include/repo_sync_manager.hpp
cpp/src/repo_sync_manager.cpp
cpp/include/components/robot_sync_banner.{hpp,cpp}
cpp/include/components/robot_sync_dialog.{hpp,cpp}
cpp/include/settings_constants.hpp   # kRobotDeployBranch, snooze key
cpp/src/app_shell.cpp                # arm + banner host stack
cpp/tests/repo_sync_tests.cpp
```

Robot helpers (do not edit from BDR_CP):

```
pilot_control/scripts/deploy/install_deploy_helpers.sh
pilot_control/scripts/deploy/rebuild_affected.sh
pilot_control/scripts/deploy/test_rebuild_affected.sh
pilot_control/scripts/deploy/robot_switch_branch.sh
```
