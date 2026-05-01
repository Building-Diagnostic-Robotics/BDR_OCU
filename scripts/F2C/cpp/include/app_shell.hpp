#pragma once

#include <QMainWindow>

class QLabel;
class QStackedWidget;

namespace f2c_cpp {

class SetupScreen;
class StartupScreen;

class AppShellWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit AppShellWindow(QWidget* parent = nullptr);

private slots:
    void onLoginSubmitted(const QString& robotId, const QString& accessCode);
    void goToStage1();
    void goToStage2();
    void goToStage3();

private:
    QWidget* buildStage3Placeholder();
    void updateStage3Labels();

    QStackedWidget* stack_ = nullptr;
    SetupScreen* stage1_ = nullptr;
    StartupScreen* stage2_ = nullptr;
    QWidget* stage3_ = nullptr;

    QString robot_id_;
    QString access_code_;  // in-memory only

    // Stage 3 placeholder labels (until stage 3 is implemented)
    QLabel* lbl_stage3_title_ = nullptr;
    QLabel* lbl_stage3_subtitle_ = nullptr;
};

}  // namespace f2c_cpp

