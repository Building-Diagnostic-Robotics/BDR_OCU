/**
 * @file camera_switch_pill.hpp
 * @brief Two-button segmented control for selecting which onboard camera
 *        UDC streams over RTP to the OCU's FPV pane.
 *
 * Drives the robot-side `/stream_camera_select` topic.  Subscribes (via
 * AppShell -> ExplorationScreen -> setActiveCamera) to
 * `/stream_camera_status` so the active button reflects what the robot
 * is ACTUALLY streaming, not what the operator just clicked.  This
 * matters because UDC's pipeline rebuild takes ~1-2 s — between click
 * and confirmation the buttons stay debounced (no further input
 * accepted) and the visual stays on the previous-confirmed state until
 * the status callback flips it.
 *
 * Visual matches the Figma reference at
 * https://www.figma.com/design/I9tRcFEniAqnXD0yiMlo6W/Untitled?node-id=196-158
 *   - Container: #27272a, 4 px padding, 8 px gap, 10 px radius.
 *   - Active button: #009966 fill, white text, 8 px radius, Arimo 14 / 20.
 *   - Inactive button: transparent, #9f9fa9 text, same geometry.
 *   - 16 px camera glyph on each button (tinted to match text color).
 *
 * Implementation notes (lessons from the first pass):
 *   - Each button uses QPushButton::setIcon() + setText() rather than
 *     a child QHBoxLayout of QLabels.  Two reasons: (a) QPushButton's
 *     sizeHint() is computed from its OWN text/icon; child layouts
 *     don't propagate, so an empty QPushButton + child layout
 *     collapses to chrome size.  (b) QLabel children intercept mouse
 *     events by default, breaking QPushButton::clicked.
 *   - No QGraphicsDropShadowEffect.  In Qt 5.15, setting any
 *     QGraphicsEffect on a child of a QStackedLayout(StackAll)
 *     transparent overlay forces an offscreen render path that
 *     intermittently clobbers the parent's transparency, hiding the
 *     other HUD pills (FPS, Speed) sitting on the same overlay.  The
 *     Figma drop-shadow is barely perceptible at this 36 px size; the
 *     #009966 vs transparent contrast carries the active-state cue
 *     on its own.
 */

#pragma once

#include <QString>
#include <QWidget>

class QPushButton;
class QTimer;

namespace f2c_cpp {

class CameraSwitchPill : public QWidget {
    Q_OBJECT

public:
    explicit CameraSwitchPill(QWidget* parent = nullptr);

    /**
     * Update the visual to mark `cam` as the active camera. Called by
     * AppShell from the `/stream_camera_status` callback so the pill
     * reflects the robot's confirmed state.  Also re-enables the
     * buttons (clears the click-debounce) once the confirmation
     * matches the most-recent request.
     *
     * `cam` must be "left" or "right"; anything else is ignored.
     */
    void setActiveCamera(const QString& cam);

    /** Currently-displayed active camera ("left" / "right"). */
    QString activeCamera() const { return active_; }

signals:
    /**
     * Emitted when the operator clicks one of the two buttons. AppShell
     * forwards to a `/stream_camera_select` publish.  `cam` is "left"
     * or "right".  Suppressed (with a debounce timer) if the operator
     * mashes the same button repeatedly or toggles faster than UDC's
     * ~1-2 s pipeline rebuild.
     */
    void cameraRequested(const QString& cam);

private:
    void buildUi();
    void onLeftClicked();
    void onRightClicked();
    void requestCamera(const QString& cam);
    // Re-applies fills + text colors for both buttons based on `active_`.
    void refreshButtonStyling();

    QPushButton* btn_left_ = nullptr;
    QPushButton* btn_right_ = nullptr;
    QString active_ = QStringLiteral("left");
    QString last_requested_;  // latest emit; cleared once confirmed by setActiveCamera
    // Disables both buttons for ~1.5 s after a click so rapid toggling
    // can't queue multiple GStreamer pipeline rebuilds on UDC.  Reset
    // earlier if setActiveCamera() confirms our request first.
    QTimer* debounce_timer_ = nullptr;
};

}  // namespace f2c_cpp
