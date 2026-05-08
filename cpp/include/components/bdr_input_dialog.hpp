/**
 * @file bdr_input_dialog.hpp
 * @brief Themed text-input dialog matching BDR dark/light styling.
 *        Sibling of BdrMessageBox; QInputDialog::getText is rejected
 *        because it ignores the BDR theme.
 *
 * Validation:
 *   The static getText() helper takes a validator callback. When the user
 *   clicks OK, the callback receives the trimmed text and returns either:
 *     - empty QString  → input is valid; dialog accepts and returns the text.
 *     - non-empty      → treated as an inline error message; the dialog
 *                        shows it (theme-aware red text below the input)
 *                        and stays open. No popup-on-popup churn.
 *
 *   The empty-validator default accepts any non-empty trimmed string.
 */

#pragma once

#include <QDialog>
#include <QString>
#include <functional>

class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;

namespace f2c_cpp {

class BdrInputDialog : public QDialog {
    Q_OBJECT

public:
    using Validator = std::function<QString(const QString&)>;

    explicit BdrInputDialog(QWidget* parent = nullptr, bool dark_mode = true);

    void setTitle(const QString& title);
    void setPrompt(const QString& prompt);
    void setDefaultText(const QString& text);
    void setValidator(Validator v);
    void setDarkMode(bool dark_mode);

    QString text() const;

    /**
     * Static helper. Returns the trimmed text on OK, empty QString on cancel
     * or close. If `accepted` is non-null, written with true on accept.
     */
    static QString getText(QWidget* parent,
                           const QString& title,
                           const QString& prompt,
                           const QString& default_text,
                           bool dark_mode,
                           Validator validator = nullptr,
                           bool* accepted = nullptr);

private slots:
    void onAccept();
    void onReject();

private:
    void buildUi();
    void applyStyle();

    bool dark_mode_ = true;
    Validator validator_;

    QLabel* lbl_title_ = nullptr;
    QPushButton* btn_close_ = nullptr;
    QLabel* lbl_prompt_ = nullptr;
    QLineEdit* edit_ = nullptr;
    QLabel* lbl_error_ = nullptr;
    QPushButton* btn_cancel_ = nullptr;
    QPushButton* btn_ok_ = nullptr;
};

}  // namespace f2c_cpp
