/**
 * @file update_settings.hpp
 * @brief QSettings keys + helpers for the OTA update flow.
 *
 * All keys live under the shared org/app from settings_constants.hpp so they
 * roundtrip cleanly with the rest of the OCU's persisted state.
 */

#pragma once

#include <QSettings>
#include <QString>

#include "settings_constants.hpp"

namespace f2c_cpp::update {

/// Epoch-ms timestamp until which banner+modal must stay hidden. 0 = no snooze.
inline constexpr const char* kKeySnoozeUntilMs = "update/snooze_until_ms";

/// Last commit SHA the user has acknowledged seeing in a banner (used to
/// suppress duplicate notifications for the same release).
inline constexpr const char* kKeyLastSeenSha = "update/last_seen_sha";

/// Master toggle for auto-checking. Default true. Reserved for a future
/// Settings UI — no UI consumer in phase 3.
inline constexpr const char* kKeyAutoCheckEnabled = "update/auto_check_enabled";

/// HTTP ETag from the most recent successful Releases API response. Sent
/// back as `If-None-Match` to opt into 304 Not Modified responses, which
/// don't count against the GitHub API rate limit.
inline constexpr const char* kKeyLastEtag = "update/last_etag";

/**
 * @brief Construct a QSettings rooted at the OCU's standard org/app pair.
 *        Caller owns the returned object; copies are fine since QSettings
 *        is value-typed.
 */
inline QSettings settings() {
    return QSettings(QString::fromLatin1(kSettingsOrgName),
                     QString::fromLatin1(kSettingsAppName));
}

}  // namespace f2c_cpp::update
