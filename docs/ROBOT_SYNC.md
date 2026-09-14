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

An actionable mismatch raises **Robot software** below the OTA banner
(OTA stays first). View Details opens a frameless modal (`show()`, not
`exec()`): **Sync now** / **Later** (4 h snooze). **Prepare robot**
appears only when Check says helpers or `updateInstead` are missing.

## Sync now

Targets `kRobotDeployBranch` on both sides. If the laptop is on
another branch and the tree is clean, Sync **switches** (never
`--force`). Then:

- `git fetch` / `merge --ff-only` when origin is ahead
- rebuild affected colcon packages on the laptop
- `git push` (explicit refspec, no `--force`) to the robot
- `~/pilot_deploy/robot_switch_branch.sh` if the robot branch differs
- `~/pilot_deploy/rebuild_affected.sh` on the robot
- verify robot HEAD == laptop HEAD

Dirty or diverged trees are hard fails. Nothing is discarded.

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
`rebuild_affected.sh`). It must not call `workspacePackageNames()` —
that walks the laptop tree and is not unit-testable in isolation.
The instance `affectedPackages()` filters the static result to
packages that actually exist.

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
pilot_control/scripts/deploy/robot_switch_branch.sh
```
