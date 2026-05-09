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
#include <QStringList>

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

/// Persisted FIFO list of short SHAs that the OCU rolled back from. The
/// UpdateChecker filters incoming releases against this list so a
/// release that has already proven to crash on this hardware is never
/// re-offered as an update — even if it remains the rolling `latest`
/// for hours before a fix-forward push. The list is bounded so a long
/// history of bad releases can't grow unbounded; oldest entries are
/// evicted first.
inline constexpr const char* kKeyDenylist = "update/denylist";

/// Hard cap on how many rolled-back versions we remember. 10 is far more
/// than will accumulate in practice (a single bad release is rare; ten
/// is "the project has more serious problems than OTA").
inline constexpr int kDenylistMaxEntries = 10;

/**
 * @brief Construct a QSettings rooted at the OCU's standard org/app pair.
 *        Caller owns the returned object; copies are fine since QSettings
 *        is value-typed.
 */
inline QSettings settings() {
    return QSettings(QString::fromLatin1(kSettingsOrgName),
                     QString::fromLatin1(kSettingsAppName));
}

/**
 * @brief Read the persisted denylist of rolled-back short SHAs.
 *        Order is FIFO (oldest first); never empty entries.
 */
inline QStringList denylist() {
    QSettings s = settings();
    return s.value(QString::fromLatin1(kKeyDenylist)).toStringList();
}

/**
 * @brief Returns true if `remoteFullOrShortSha` matches any denylist entry
 *        as a case-insensitive prefix. Stored entries are typically 7-char
 *        short SHAs; remote release SHAs from the GitHub API are 40 chars.
 *        Prefix-matching against the stored entry's length handles both.
 */
inline bool isDenylisted(const QString& remoteFullOrShortSha) {
    if (remoteFullOrShortSha.isEmpty()) return false;
    const QStringList list = denylist();
    for (const QString& entry : list) {
        if (entry.isEmpty()) continue;
        if (remoteFullOrShortSha.left(entry.size())
                .compare(entry, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Append `shortSha` to the denylist (idempotent, capped FIFO).
 *        No-op for empty input. Calls QSettings::sync() so the entry is
 *        durable on disk before the caller hands off control to the
 *        update runner — this matters for the rollback path, where the
 *        OCU may exec out moments after recording the bad SHA.
 */
inline void addToDenylist(const QString& shortSha) {
    if (shortSha.isEmpty()) return;
    QSettings s = settings();
    QStringList list =
        s.value(QString::fromLatin1(kKeyDenylist)).toStringList();
    for (const QString& entry : list) {
        if (entry.compare(shortSha, Qt::CaseInsensitive) == 0) {
            return;  // already present; keep its FIFO position
        }
    }
    list.append(shortSha);
    while (list.size() > kDenylistMaxEntries) list.removeFirst();
    s.setValue(QString::fromLatin1(kKeyDenylist), list);
    s.sync();
}

/**
 * @brief Extract the short git SHA from a packaged .deb path produced by
 *        `cpp/create_deb.sh`. Filenames follow the canonical pattern:
 *
 *            bdr-coverage-planner_<semver>-<build>+<short_sha>_<arch>.deb
 *
 *        Returns the substring after the last '+' and before the
 *        '_<arch>.deb' suffix, or an empty string if the path doesn't
 *        match the expected shape (legacy / hand-built debs).
 */
inline QString shortShaFromDebPath(const QString& debPath) {
    if (debPath.isEmpty()) return {};
    QString name = debPath;
    const int slash = name.lastIndexOf(QLatin1Char('/'));
    if (slash >= 0) name = name.mid(slash + 1);
    if (name.endsWith(QStringLiteral(".deb"))) name.chop(4);
    const int us = name.lastIndexOf(QLatin1Char('_'));
    if (us >= 0) name.truncate(us);
    const int plus = name.lastIndexOf(QLatin1Char('+'));
    if (plus < 0) return {};
    return name.mid(plus + 1);
}

}  // namespace f2c_cpp::update
