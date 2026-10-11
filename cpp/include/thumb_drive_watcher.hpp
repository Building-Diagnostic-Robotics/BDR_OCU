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
 *  3. `/media/<user>/<label>`, `/run/media/<user>/<label>`, `/media/<label>`
 *     and `/mnt/<label>` as a fallback for sticks mounted by name without
 *     a readable by-label symlink. A candidate counts only when
 *     `/proc/self/mounts` lists that path — a plain directory with the
 *     label as its name is not a data source.
 *
 * There is no operator-chosen folder. Scan data comes from this stick.
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
        Absent = 0,         ///< No mounted volume with the label.
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

    /// True when `path` is the mount point of some line in `mounts_text`
    /// (`/proc/self/mounts` shape). A directory that merely exists is not
    /// enough — that is what stops a local folder named `RDATA_EXT` from
    /// being treated as the stick.
    static bool isMountPoint(const QString& path, const QString& mounts_text);

signals:
    /// Emitted whenever `state()` or `mountPath()` changes.
    void stateChanged(ThumbDriveWatcher::State state, const QString& mount_path);

private:
    void evaluate();
    void applyState(State state, const QString& mount_path);
    void requestMount(const QString& device);

    QString label_;
    State state_ = State::Absent;
    QString mount_path_;
    QTimer* timer_ = nullptr;

    // udisks mount attempt bookkeeping: one attempt per device node so a
    // stick that refuses to mount doesn't spawn udisksctl every 2 s.
    QString mount_attempted_device_;
    QPointer<QProcess> mount_proc_;
};

}  // namespace f2c_cpp
