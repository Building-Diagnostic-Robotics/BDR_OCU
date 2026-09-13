/**
 * @file thumb_drive_watcher.hpp
 * @brief Polls for the RDATA_EXT thumb drive on the OCU laptop.
 *
 * The robot's offload worker (`rdata_offload.py` / `rdata_thumbdrive_sync.py`
 * on the robot) copies every finalized mission onto a USB stick labelled
 * `RDATA_EXT`, mirroring `/R_DATA/<date>/<building>/{Mission_*,Section_*}`.
 * The operator then walks that stick to the laptop, and `UploadDialog`
 * uploads from it. This class answers one question for the dialog, every
 * two seconds: *where is that stick mounted right now?*
 *
 * Detection order (all pure file I/O — no subprocess on the GUI thread):
 *  1. `/dev/disk/by-label/<label>` → block device → matching mount in
 *     `/proc/self/mounts`. This is how the robot side finds the drive too.
 *  2. If the device exists but is not mounted (no desktop automount, e.g.
 *     a locked session), ask udisks to mount it — `udisksctl mount -b`
 *     runs as the logged-in user, no sudo — once per insertion, then
 *     re-poll.
 *  3. `/media/<user>/<label>` and `/run/media/<user>/<label>` as a fallback
 *     for sticks mounted by name without a readable by-label symlink.
 *  4. An operator-chosen folder (`setManualPath`, the dialog's Browse…)
 *     wins over everything while it exists — for an unlabelled stick or a
 *     copy already pulled onto local disk.
 */

#pragma once

#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>

class QTimer;

namespace f2c_cpp {

class ThumbDriveWatcher : public QObject {
    Q_OBJECT
public:
    enum class State {
        Absent = 0,         ///< No device with the label, no manual path.
        PresentUnmounted,   ///< Device present, mount attempt in flight.
        Mounted,            ///< `mountPath()` is a usable data root.
    };
    Q_ENUM(State)

    static constexpr int kPollIntervalMs = 2000;
    static const char* const kDefaultLabel;   // "RDATA_EXT"

    explicit ThumbDriveWatcher(QObject* parent = nullptr);
    ~ThumbDriveWatcher() override;

    /// Filesystem label to look for. Defaults to `RDATA_EXT` — must match
    /// the `thumbdrive_copy_label` parameter on the robot's director.
    void setLabel(const QString& label);
    QString label() const { return label_; }

    /// Operator override (Browse…). Empty clears it. Takes effect on the
    /// next poll, which `start()`/`poll()` trigger immediately.
    void setManualPath(const QString& path);
    QString manualPath() const { return manual_path_; }

    void start();
    void stop();
    bool isActive() const;

    /// Force an immediate re-evaluation (also emits on change).
    void poll();

    State state() const { return state_; }
    QString mountPath() const { return mount_path_; }
    bool isMounted() const { return state_ == State::Mounted; }

    /// Free bytes on the mounted volume, or -1 when not mounted.
    qint64 freeBytes() const;

    /// Pure resolver — exposed for tests. Returns the mount point of the
    /// block device `device` per `/proc/self/mounts` (or the given mounts
    /// text), empty if not mounted. Handles the kernel's octal escapes.
    static QString mountPointForDevice(const QString& device,
                                       const QString& mounts_text);

signals:
    /// Emitted whenever `state()` or `mountPath()` changes.
    void stateChanged(ThumbDriveWatcher::State state, const QString& mount_path);

private:
    void evaluate();
    void applyState(State state, const QString& mount_path);
    void requestMount(const QString& device);

    QString label_;
    QString manual_path_;
    State state_ = State::Absent;
    QString mount_path_;
    QTimer* timer_ = nullptr;

    // udisks mount attempt bookkeeping: one attempt per device node so a
    // stick that refuses to mount doesn't spawn udisksctl every 2 s.
    QString mount_attempted_device_;
    QPointer<QProcess> mount_proc_;
};

}  // namespace f2c_cpp
