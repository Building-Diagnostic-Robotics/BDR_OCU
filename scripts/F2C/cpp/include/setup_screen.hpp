#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QToolButton;

namespace f2c_cpp {

class SetupScreen : public QWidget {
    Q_OBJECT

public:
    explicit SetupScreen(QWidget* parent = nullptr);

signals:
    void loginSubmitted(const QString& robotId, const QString& accessCode);

private slots:
    void updateUiState();
    void toggleAccessCodeVisible();
    void submit();

private:
    void applyLocalStyle();

    QLineEdit* txt_robot_id_ = nullptr;
    QLineEdit* txt_access_code_ = nullptr;
    QToolButton* btn_view_code_ = nullptr;
    QToolButton* btn_arrow_login_ = nullptr;
    QLabel* lbl_error_ = nullptr;

    bool access_code_visible_ = false;
};

}  // namespace f2c_cpp

