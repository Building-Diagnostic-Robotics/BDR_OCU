/**
 * @file bdr_message_box.hpp
 * @brief Custom frameless message box matching BDR dark-theme design.
 *        Drop-in replacement for QMessageBox.
 */

#pragma once

#include <QDialog>

class QHBoxLayout;
class QLabel;
class QPushButton;

namespace f2c_cpp {

class BdrMessageBox : public QDialog {
    Q_OBJECT

public:
    /** Standard button roles (values compatible with QMessageBox::StandardButton for migration). */
    enum StandardButton {
        Ok       = 1024,
        Cancel   = 4194304,
        Yes      = 16384,
        No       = 65536,
    };

    explicit BdrMessageBox(QWidget* parent = nullptr);

    void setTitle(const QString& title);
    void setText(const QString& text);
    void setInformativeText(const QString& text);
    void setIconPath(const QString& path, const QString& strokeColor = QString());

    /** Add a button; returns the button's index (0-based). */
    int addButton(const QString& label, bool isPrimary = false);

    /** Static helpers (drop-in QMessageBox replacements). */
    static int information(QWidget* parent, const QString& title, const QString& text);
    static int warning(QWidget* parent, const QString& title, const QString& text);
    static int critical(QWidget* parent, const QString& title, const QString& text);
    static int question(QWidget* parent, const QString& title, const QString& text,
                        int defaultButton = No);

    /**
     * Custom dialog with multiple buttons.
     * @return Index of clicked button (0-based), or -1 if dialog was rejected.
     */
    static int custom(QWidget* parent, const QString& title, const QString& text,
                      const QStringList& buttonLabels, int defaultIndex = -1,
                      const QString& informativeText = QString());

    /** Returns the clicked button index (0-based) or -1 if rejected. */
    int clickedButtonIndex() const { return result_; }

private slots:
    void onCloseClicked();
    void onButtonClicked(int index);

private:
    void buildUi();
    void applyStyle();

    QLabel* lbl_title_ = nullptr;
    QPushButton* btn_close_ = nullptr;
    QLabel* lbl_icon_ = nullptr;
    QLabel* lbl_text_ = nullptr;
    QWidget* button_box_ = nullptr;
    QHBoxLayout* button_layout_ = nullptr;

    QString informative_text_;
    QList<QPushButton*> buttons_;
    int result_ = -1;
};

}  // namespace f2c_cpp
