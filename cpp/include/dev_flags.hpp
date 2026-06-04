#pragma once

// =============================================================================
// BDR_REWIRE: Unified dev-mode gate bypass.
//
// When the CMake option BDR_DEV_MODE is ON (only permitted in non-Release
// build types — see cpp/CMakeLists.txt), the BDR_DEV_MODE macro is defined and
// kDevMode is true. Gate sites guard their bypass with `if constexpr (kDevMode)`
// so that in a Release build (where the macro is never defined) the bypass code
// is compiled out entirely — zero runtime cost, zero attack surface, and no way
// to accidentally ship a gate-bypassed binary.
//
// What dev mode does (see docs/DEV_BYPASSES.md for the full site list):
//   - Stage 1 login: SSH login is still attempted, but a failure no longer
//     blocks advancing to Stage 2.
//   - Stage 3 -> 4: the New Scan metadata modal still shows, but Cancel/X
//     proceeds to Stage 4 instead of trapping the operator.
//   - Stage 4: robot launch is still attempted, but the planner button is
//     unlocked regardless of launch result.
//   - Stage 5 Start Scan: UDC-health / metadata-push / autonomy calls are
//     still attempted, but a failure no longer hard-blocks the run. NOTE: dev
//     mode CAN command a real robot if one is connected.
//   - Link-offline grey-out: disabled so controls stay live even when the
//     (still-armed) link monitor reports stale topics.
//   - Folds the previously-hardcoded bypasses (Stage 2 preflight passthrough,
//     planner stage gates, Stage 4->5 map override, Complete Mission) under the
//     same flag so a Release build is fully gated again.
//
// To re-enable all gating: build Release (or simply don't pass -DBDR_DEV_MODE=ON).
// =============================================================================

#ifdef BDR_DEV_MODE
inline constexpr bool kDevMode = true;
#else
inline constexpr bool kDevMode = false;
#endif
