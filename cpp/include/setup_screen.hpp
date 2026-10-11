#pragma once

#include <QHash>
#include <QPixmap>
#include <QProcess>
#include <QString>
#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;
class QTimer;

namespace f2c_cpp {

/**
 * Stage 1 "Get started". The robot comes from robots.json (see
 * `RobotRegistry::pairedRobotId`), not from a typed ID and PIN, so the
 * Dashboard — and with it planning and thumb-drive upload — opens with the
 * robot powered off. A laptop whose file lists several robots and names
 * none gets a picker instead.
 */
class SetupScreen : public QWidget {
    Q_OBJECT

public:
    explicit SetupScreen(QWidget* parent = nullptr);
    void setDarkMode(bool dark_mode);

    /** True when the operator chooses the robot here (several robots, none paired). */
    bool robotIsPickable() const { return robot_picker_ != nullptr; }

signals:
    void getStartedRequested(const QString& robotId);

private slots:
    void submit();
    void startConnectionMonitor();
    void checkConnection();
    void onPingFinished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    QString selectedRobotId() const;
    void applyLocalStyle();
    void updateConnectionUi(bool connected);

    QLabel* lbl_robot_value_ = nullptr;
    QComboBox* robot_picker_ = nullptr;
    QPushButton* btn_start_ = nullptr;
    QLabel* lbl_override_ = nullptr;
    QLabel* lbl_error_ = nullptr;

    QLabel* lbl_connection_ = nullptr;
    QLabel* lbl_connection_icon_ = nullptr;

    QProcess* ping_proc_ = nullptr;
    QTimer* ping_timer_ = nullptr;
    QPixmap wireframe_connected_;
    QPixmap wireframe_disconnected_;

    QString paired_robot_id_;
    /// robot_id -> host, so the CONNECTED indicator pings the robot that
    /// Get started will actually select.
    QHash<QString, QString> host_by_id_;

    bool dark_mode_ = false;
    bool last_connection_state_valid_ = false;
    bool last_connection_state_ = false;
};

}  // namespace f2c_cpp
