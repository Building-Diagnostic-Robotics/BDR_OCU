/**
 * @file bdr_progress_dialog.hpp
 * @brief Custom frameless progress dialog matching BDR dark-theme design.
 *        Replacement for QProgressDialog.
 */

#pragma once

#include <QDialog>

class QLabel;
class QProgressBar;
class QPushButton;

namespace f2c_cpp {

class BdrProgressDialog : public QDialog {
    Q_OBJECT

public:
    explicit BdrProgressDialog(QWidget* parent = nullptr);

    void setTitle(const QString& title);
    void setLabelText(const QString& text);
    void setRange(int minimum, int maximum);
    void setValue(int value);
    void setCancelButton(QPushButton* button);
    void setMinimumDuration(int ms);

    int value() const;
    int minimum() const;
    int maximum() const;

    /** Show success message, fill progress bar, and auto-close after 2.5s. */
    void showSuccessAndClose(const QString& message);

private slots:
    void onCloseTimerFired();

private:
    void buildUi();
    void applyStyle();

    QLabel* lbl_title_ = nullptr;
    QPushButton* btn_close_ = nullptr;
    QLabel* lbl_status_ = nullptr;
    QProgressBar* progress_bar_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
};

}  // namespace f2c_cpp
