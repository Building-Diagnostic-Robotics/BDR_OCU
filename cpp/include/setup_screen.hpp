#pragma once

#include <QPixmap>
#include <QProcess>
#include <QString>
#include <QWidget>
#include <utility>

class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;
class QTimer;

namespace f2c_cpp {

class SetupScreen : public QWidget {
    Q_OBJECT

public:
    explicit SetupScreen(QWidget* parent = nullptr);
    void setDarkMode(bool dark_mode);

signals:
    void loginSubmitted(const QString& robotId, const QString& accessCode);

private slots:
    void updateUiState();
    void toggleAccessCodeVisible();
    void submit();
    void startConnectionMonitor();
    void checkConnection();
    void onPingFinished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    /** Returns trimmed robot ID and access code; single source of truth for validation. */
    std::pair<QString, QString> getTrimmedCredentials() const;

    void applyLocalStyle();
    void updateConnectionUi(bool connected);

    QLineEdit* txt_robot_id_ = nullptr;
    QLineEdit* txt_access_code_ = nullptr;
    QToolButton* btn_view_code_ = nullptr;
    QPushButton* btn_login_ = nullptr;
    QLabel* lbl_error_ = nullptr;

    QLabel* lbl_connection_ = nullptr;
    QLabel* lbl_connection_icon_ = nullptr;

    QProcess* ping_proc_ = nullptr;
    QTimer* ping_timer_ = nullptr;
    QPixmap wireframe_connected_;
    QPixmap wireframe_disconnected_;

    /// Cached host IP of the first robot in robots.json, resolved once at
    /// construction. Drives the connection-status indicator (top-left
    /// CONNECTED/DISCONNECTED) which reflects radio-link reachability —
    /// independent of whatever Robot ID the operator has typed.
    QString connection_target_host_;

    bool dark_mode_ = false;
    bool last_connection_state_valid_ = false;
    bool last_connection_state_ = false;
};

}  // namespace f2c_cpp

