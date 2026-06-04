/**
 * @file update_types.hpp
 * @brief Shared OTA value types + tunable constants.
 *
 * Pure data types — no Qt/networking deps beyond QString. Lives in its own
 * header so unit tests can include it without dragging in QNetworkAccessManager.
 */

#pragma once

#include <QString>

namespace f2c_cpp::update {

/**
 * @brief Snapshot of a remote release as parsed from the GitHub Releases API.
 *
 * Populated by UpdateChecker on a successful poll, then handed off to the
 * banner / modal / downloader. All fields are best-effort; the OCU OTA flow
 * treats missing optional fields (sha256Url, sizeBytes) gracefully.
 */
struct VersionInfo {
    /// Release tag, e.g. "v-fb662d4" or "latest".
    QString tag;

    /// Resolved commit SHA for the release (see UpdateChecker::extractRemoteSha:
    /// release `name` → `v-<sha>` tag → `target_commitish` only if it's a real
    /// SHA). Empty when none is usable. Compared against the embedded short SHA
    /// via prefix match.
    QString commitSha;

    /// Direct browser_download_url to the `_amd64.deb` asset.
    QString downloadUrl;

    /// Asset filename (e.g. "bdr-coverage-planner_1.0.0-127+fb662d4_amd64.deb").
    /// Useful for display + as the lookup key for the SHA256 sidecar.
    QString assetName;

    /// browser_download_url to the matching `<asset>.sha256` sidecar (may be
    /// empty if CI didn't publish one — UpdateDownloader will then fail-closed).
    QString sha256Url;

    /// Body of the release as authored on GitHub (markdown). Modal renders
    /// only lines starting with `-` from this; raw text is kept for logs.
    QString releaseNotes;

    /// Size of the .deb asset in bytes (0 if the API didn't include it).
    qint64 sizeBytes = 0;

    /// Release publish time in UTC (from GitHub `published_at`). Empty if
    /// the API didn't include it. Modal renders this as a localized date.
    QString publishedAtIso8601;
};

/// How often to poll GitHub Releases when no in-flight check is running.
constexpr int kPollIntervalMs = 5 * 60 * 1000;  // 5 minutes

/// Delay between app start and the first poll (avoids startup network thrash).
constexpr int kFirstCheckDelayMs = 30 * 1000;  // 30 s

/// Per-request HTTP timeout. Aborts the QNetworkReply on expiry.
constexpr int kRequestTimeoutMs = 10 * 1000;  // 10 s

/// Initial backoff applied after a failed check; doubles up to the cap.
constexpr int kBackoffStartMs = kPollIntervalMs;

/// Maximum backoff between retries when the network is flapping.
constexpr int kBackoffMaxMs = 30 * 60 * 1000;  // 30 minutes

/// Snooze duration when the operator clicks "Remind Me Later" in the modal.
constexpr qint64 kSnoozeDurationMs = 4LL * 60LL * 60LL * 1000LL;  // 4 hours

/// Asset filename suffix used to identify the OTA Debian package.
inline constexpr const char* kDebAssetSuffix = "_amd64.deb";

/// SHA256 sidecar suffix appended to the .deb asset name.
inline constexpr const char* kSha256SidecarSuffix = ".sha256";

/// User-Agent header value sent on every GitHub API request. Required by
/// api.github.com or the request is rejected.
inline constexpr const char* kUserAgent = "BDR_OCU-UpdateChecker/1.0";

// ---------------------------------------------------------------------------
// UpdateDownloader robustness knobs (locked phase 5.5)
// ---------------------------------------------------------------------------

/// Stall timer: abort + auto-retry if no readyRead arrives for this long.
constexpr int kStallTimeoutMs = 120 * 1000;  // 2 minutes

/// Maximum number of automatic retry attempts after the initial download.
/// Total attempts = 1 + kMaxRetries. Each restarts from byte 0.
constexpr int kMaxRetries = 3;

/// Base delay before retry attempt N: kRetryBaseMs * 3^(N-1).
/// 5 s, 15 s, 45 s — keeps the worst-case retry wall under 70 s.
constexpr int kRetryBaseMs = 5 * 1000;

/// Hard ceiling on total wall-time for a single .deb fetch (sum of all
/// in-attempt time, not counting retry sleep). Defends against pathological
/// trickle-feed servers that slip past the stall timer.
constexpr int kDebTotalCeilingMs = 15 * 60 * 1000;  // 15 minutes

/// Cooldown applied when GitHub returns 403 (rate-limited). Polls suspended
/// for this long; resumes automatically.
constexpr int kRateLimitCooldownMs = 60 * 60 * 1000;  // 1 hour

/// Feature flag for HTTP Range resume on retry. Off in v1 — restart-from-0
/// is simpler and the deployment plan (Toughbook field-office wifi) makes
/// the bandwidth savings marginal. Re-evaluate after first field deployment.
inline constexpr bool kRangeResumeEnabled = false;

}  // namespace f2c_cpp::update
