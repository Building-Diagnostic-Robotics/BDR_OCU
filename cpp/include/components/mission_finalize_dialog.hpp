/**
 * @file mission_finalize_dialog.hpp
 * @brief Stage 6 Complete Mission progress modal (reachable-link path).
 *
 * The director owns the end-of-run sequence (/coverage/conclude ->
 * /dc/end_and_save -> /dc/finalize_mission -> thumb-drive copy), and reports
 * it through /coverage/status. This modal shows that sequence as phases the
 * operator can follow and carries the two decisions that can arise:
 *
 *   - **Skip copy** while the copy is pending / copying / failed
 *     (/coverage/skip_copy). A dead USB port must not strand the robot.
 *   - **Abort & save partial** when the director refuses to conclude
 *     (coverage still active after autonomy was dropped).
 *
 * The screen drives it (setPhase / setCopyState / finish); the dialog never
 * talks to ROS itself. Application-modal but shown with show(), not exec(),
 * so the screen's async callbacks keep flowing without a nested loop.
 */

#pragma once

#include <QDialog>
#include <QString>

class QLabel;
class QProgressBar;
class QPushButton;

namespace f2c_cpp {

class MissionFinalizeDialog : public QDialog {
    Q_OBJECT

public:
    explicit MissionFinalizeDialog(QWidget* parent = nullptr);

    /** Header line — the modal also carries plain teardowns, not just
        Complete Mission. */
    void setTitle(const QString& text);
    /** Current phase line, e.g. "Concluding coverage…". */
    void setPhase(const QString& text);
    /** Secondary detail (copy progress, error text). Empty hides it. */
    void setDetail(const QString& text, bool is_error = false);
    /** Shows / hides the Skip-copy CTA. */
    void setSkipCopyAvailable(bool available);
    /** Shows / hides the Abort-and-save CTA. */
    void setAbortAvailable(bool available);
    /**
     * Shows / hides the Force-stop CTA — the escape hatch when teardown is
     * waiting on a robot that has stopped answering. The screen only offers
     * it once the wait has gone long enough to be worth cutting short.
     */
    void setForceStopAvailable(bool available);
    /** Terminal: swaps the CTAs for Close and stops the busy bar. */
    void finish(bool ok, const QString& summary);

signals:
    void skipCopyRequested();
    void abortAndSaveRequested();
    void forceStopRequested();

private:
    QLabel* lbl_header_ = nullptr;
    QLabel* lbl_phase_ = nullptr;
    QLabel* lbl_detail_ = nullptr;
    QProgressBar* busy_ = nullptr;
    QPushButton* btn_skip_copy_ = nullptr;
    QPushButton* btn_abort_ = nullptr;
    QPushButton* btn_force_ = nullptr;
    QPushButton* btn_close_ = nullptr;
};

}  // namespace f2c_cpp
