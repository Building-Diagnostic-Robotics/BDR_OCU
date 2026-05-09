/**
 * @file update_log.hpp
 * @brief Persistent on-disk logger for the OTA flow.
 *
 * Why this exists: the OCU runs on a Toughbook in the field, often without
 * a terminal attached. `std::cout` from OTA paths vanishes. This logger
 * appends to `<CacheLocation>/update.log`, rotates at 1 MB, keeps 5 rotated
 * backups (`.log.1` … `.log.5`), and tees the same line to `std::cerr` for
 * dev visibility.
 *
 * Cross-process safety:
 *   - The OCU and the bdr-update-runner BOTH write to the same update.log
 *     (per locked spec phase 7 concern #1).
 *   - Writes go through a single low-level `::write()` syscall on a file
 *     opened with O_APPEND. POSIX guarantees that O_APPEND writes <= PIPE_BUF
 *     (4096 bytes on Linux) are atomic across processes. Our log lines are
 *     well under that ceiling.
 *   - Each process tags its lines with a short identifier ("ocu" / "runner")
 *     via setProcessTag() so the merged log is debuggable.
 *   - Rotation is best-effort: if both processes race the rename(), at most
 *     one line is written to the rotated file before the new one is opened.
 *     Acceptable for a debug log.
 *
 * Cloud upload: log entries are line-per-event plain text so a future
 * `update.log` ingestion job can tail or batch-upload without parsing.
 */

#pragma once

#include <QString>

namespace f2c_cpp::update::log {

/// Maximum size of `update.log` before rotation kicks in.
constexpr qint64 kRotateThresholdBytes = 1LL * 1024LL * 1024LL;  // 1 MB

/// Number of rotated copies to retain (`update.log.1` … `update.log.5`).
constexpr int kMaxRotations = 5;

/**
 * Set the short process tag printed on every log line, e.g. "ocu" or
 * "runner". Call once at process startup before any info/warn/error.
 * Tag must remain valid for the lifetime of the process (string-literal
 * passed in is the typical use).
 */
void setProcessTag(const char* tag);

void info(const char* component, const QString& msg);
void warn(const char* component, const QString& msg);
void error(const char* component, const QString& msg);

/// Returns the absolute path of the current log file. Useful for "Reveal log"
/// affordances and for the future cloud-upload job. Empty on first call only
/// if the cache dir cannot be created.
QString currentLogPath();

}  // namespace f2c_cpp::update::log
