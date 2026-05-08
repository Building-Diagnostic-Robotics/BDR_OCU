/**
 * @file pre_scan_checklist_dialog.cpp
 * @brief Implementation of the pre-scan operator checklist modal.
 */

#include "components/pre_scan_checklist_dialog.hpp"

#include "ui_theme_constants.hpp"

#include <QAbstractItemView>
#include <QCloseEvent>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QShowEvent>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>

namespace f2c_cpp {

namespace {

constexpr int kRowHeight = 92;
constexpr int kCheckboxSize = 22;
constexpr int kCheckboxLeftMargin = 18;

// Per-row widget that owns ALL visual state for one checklist row:
//   - paints its own check box on the left (delegate paint is bypassed
//     by setItemWidget, so we can't rely on the QStyledItemDelegate to
//     do it)
//   - paints a hover background tint
//   - toggles the underlying QListWidgetItem selection on click and
//     repaints itself (QListWidget doesn't repaint cells covered by an
//     item widget on selection change)
// Children (e.g. the Wake GPR button) receive their own clicks first
// because Qt event-routes child-first.
class ChecklistRowWidget : public QWidget {
public:
    ChecklistRowWidget(QListWidgetItem* item, bool dark_mode,
                       QWidget* parent = nullptr)
        : QWidget(parent), item_(item), dark_mode_(dark_mode) {
        setCursor(Qt::PointingHandCursor);
        setMouseTracking(true);
        // Opaque so the QListWidget cell behind doesn't show through —
        // this widget owns every pixel it covers.
        setAttribute(Qt::WA_OpaquePaintEvent, true);
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && item_) {
            item_->setSelected(!item_->isSelected());
            update();
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void enterEvent(QEvent* event) override {
        hovered_ = true;
        update();
        QWidget::enterEvent(event);
    }

    void leaveEvent(QEvent* event) override {
        hovered_ = false;
        update();
        QWidget::leaveEvent(event);
    }

    void paintEvent(QPaintEvent* /*event*/) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const bool ticked = item_ && item_->isSelected();

        // Background: panel base, plus hover tint when mouse over.
        const QColor base = dark_mode_ ? QColor("#111827") : QColor("#FFFFFF");
        const QColor hover_tint = dark_mode_ ? QColor("#1F2937")
                                             : QColor("#F1F5F9");
        painter.fillRect(rect(), hovered_ ? hover_tint : base);

        // Check box geometry.
        const int cx = kCheckboxLeftMargin + kCheckboxSize / 2;
        const int cy = height() / 2;
        const QRect box(cx - kCheckboxSize / 2, cy - kCheckboxSize / 2,
                        kCheckboxSize, kCheckboxSize);

        const QColor box_border = dark_mode_ ? QColor("#6B7280")
                                             : QColor("#9CA3AF");
        const QColor box_fill_unticked = dark_mode_ ? QColor("#1F2937")
                                                    : QColor("#FFFFFF");
        const QColor accent = dark_mode_ ? QColor("#10B981")
                                         : QColor("#155DFC");

        QPainterPath box_path;
        box_path.addRoundedRect(box, 4, 4);
        painter.fillPath(box_path, ticked ? accent : box_fill_unticked);
        painter.setPen(QPen(ticked ? accent : box_border, 1.5));
        painter.drawPath(box_path);

        if (ticked) {
            QPen pen(QColor("#FFFFFF"), 2.4);
            pen.setCapStyle(Qt::RoundCap);
            pen.setJoinStyle(Qt::RoundJoin);
            painter.setPen(pen);
            QPainterPath p;
            p.moveTo(box.left() + 5,  box.center().y() + 1);
            p.lineTo(box.center().x() - 1, box.bottom() - 5);
            p.lineTo(box.right() - 4, box.top() + 5);
            painter.drawPath(p);

            // Left accent stripe groups the ticked rows visually.
            const QRect stripe(2, 8, 3, height() - 16);
            painter.fillRect(stripe, accent);
        }
    }

private:
    QListWidgetItem* item_ = nullptr;
    bool dark_mode_ = true;
    bool hovered_ = false;
};

}  // namespace

PreScanChecklistDialog::PreScanChecklistDialog(bool dark_mode, QWidget* parent)
    : QDialog(parent), dark_mode_(dark_mode) {
    // No close button (per design — operator must confirm or stay
    // blocked). Frameless to match BDR dialog style. Application modal
    // so it blocks the entire window, not just the parent widget.
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setWindowModality(Qt::ApplicationModal);
    buildUi();
    applyStyle();
}

void PreScanChecklistDialog::buildUi() {
    auto* container = new QWidget(this);
    container->setObjectName("PreScanChecklistContainer");
    container->setAttribute(Qt::WA_StyledBackground, true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(container);

    auto* main_layout = new QVBoxLayout(container);
    main_layout->setContentsMargins(0, 0, 0, 0);
    main_layout->setSpacing(0);

    // Header (title + subtitle, no close button by design).
    auto* header = new QWidget(container);
    header->setObjectName("PreScanChecklistHeader");
    header->setAttribute(Qt::WA_StyledBackground, true);
    auto* header_layout = new QVBoxLayout(header);
    header_layout->setContentsMargins(28, 22, 28, 18);
    header_layout->setSpacing(6);

    lbl_title_ = new QLabel(QStringLiteral("Before you scan"), header);
    lbl_title_->setObjectName("PreScanChecklistTitle");
    header_layout->addWidget(lbl_title_);

    lbl_subtitle_ = new QLabel(
        QStringLiteral("Confirm each item below before starting the autonomous scan. "
                       "All five must be checked to continue."),
        header);
    lbl_subtitle_->setObjectName("PreScanChecklistSubtitle");
    lbl_subtitle_->setWordWrap(true);
    header_layout->addWidget(lbl_subtitle_);

    main_layout->addWidget(header);

    // Checklist body: QListWidget with multi-select + custom delegate
    // (matches the splitting-stage segment list look).
    list_ = new QListWidget(container);
    list_->setObjectName("PreScanChecklistList");
    list_->setSelectionMode(QAbstractItemView::MultiSelection);
    list_->setFocusPolicy(Qt::StrongFocus);
    list_->setAttribute(Qt::WA_StyledBackground, true);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setMinimumHeight(kRowHeight * 5 + 12);
    list_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    list_->setUniformItemSizes(false);

    const QStringList rows = {
        QStringLiteral("Confirm the GPR is connected. If it shows as disconnected, wake it from sleep mode using the Wake GPR button to the right."),
        QStringLiteral("After the GPR connects, verify the Proceq app is on its home screen and ready to record."),
        QStringLiteral("Confirm the scan area is clear of moving people, vehicles, and other obstacles."),
        QStringLiteral("Stay at the OCU and monitor the live scan view throughout the run."),
        QStringLiteral("Verify the planned coverage path is unobstructed and that all static obstacles have been marked correctly."),
    };

    // Indent text so it sits to the right of the painted check box.
    const int text_left_pad = kCheckboxLeftMargin + kCheckboxSize + 18;
    const int text_right_pad = 18;
    const QString label_color = dark_mode_ ? QStringLiteral("#F3F4F6")
                                           : QStringLiteral("#111827");
    const QString row_label_qss = QString(
        "background: transparent; color: %1; font-family: 'Arimo'; "
        "font-size: 14px; font-weight: 500;")
        .arg(label_color);

    for (int i = 0; i < rows.size(); ++i) {
        auto* item = new QListWidgetItem(list_);
        item->setSizeHint(QSize(0, kRowHeight));

        // ChecklistRowWidget owns its own paint (check box + hover bg)
        // and toggles selection on click. Wake GPR button (if present)
        // consumes its own clicks first.
        auto* row_widget = new ChecklistRowWidget(item, dark_mode_, list_);
        auto* row_layout = new QHBoxLayout(row_widget);
        row_layout->setContentsMargins(text_left_pad, 12,
                                       (i == 0 ? 16 : text_right_pad), 12);
        row_layout->setSpacing(12);

        auto* lbl = new QLabel(rows.at(i), row_widget);
        lbl->setObjectName("PreScanChecklistRowText");
        lbl->setWordWrap(true);
        lbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        // Apply color directly — QSS via objectName doesn't reliably
        // reach labels inside setItemWidget composites.
        lbl->setStyleSheet(row_label_qss);
        // Don't let the label intercept clicks; let them bubble to the
        // ChecklistRowWidget so any tap on the text toggles the row.
        lbl->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        row_layout->addWidget(lbl, 1);

        if (i == 0) {
            btn_wake_ = new QPushButton(QStringLiteral("Wake GPR"), row_widget);
            btn_wake_->setObjectName("PreScanChecklistWakeBtn");
            btn_wake_->setCursor(Qt::PointingHandCursor);
            btn_wake_->setFixedHeight(36);
            btn_wake_->setMinimumWidth(150);
            connect(btn_wake_, &QPushButton::clicked, this,
                    &PreScanChecklistDialog::onWakeClicked);
            row_layout->addWidget(btn_wake_, 0, Qt::AlignVCenter);
        }

        list_->setItemWidget(item, row_widget);
    }

    connect(list_, &QListWidget::itemSelectionChanged, this,
            &PreScanChecklistDialog::onRowSelectionChanged);
    main_layout->addWidget(list_, 1);

    // Footer: Confirm button (disabled until all 5 rows ticked).
    auto* footer = new QWidget(container);
    footer->setObjectName("PreScanChecklistFooter");
    footer->setAttribute(Qt::WA_StyledBackground, true);
    auto* footer_layout = new QHBoxLayout(footer);
    footer_layout->setContentsMargins(28, 18, 28, 22);
    footer_layout->setSpacing(12);
    footer_layout->addStretch(1);

    btn_confirm_ = new QPushButton(QStringLiteral("Confirm and continue"), footer);
    btn_confirm_->setObjectName("PreScanChecklistConfirmBtn");
    btn_confirm_->setCursor(Qt::PointingHandCursor);
    btn_confirm_->setFixedHeight(40);
    btn_confirm_->setMinimumWidth(220);
    btn_confirm_->setEnabled(false);
    btn_confirm_->setProperty("primary", true);
    // Make Enter trigger Confirm once enabled. AutoDefault is on for
    // any QPushButton inside a QDialog by default; setDefault marks
    // this one as the activate-on-Enter target.
    btn_confirm_->setDefault(true);
    btn_confirm_->setAutoDefault(true);
    connect(btn_confirm_, &QPushButton::clicked, this,
            &PreScanChecklistDialog::onConfirmClicked);
    footer_layout->addWidget(btn_confirm_, 0, Qt::AlignRight | Qt::AlignVCenter);

    main_layout->addWidget(footer);

    setMinimumWidth(640);
    setMinimumHeight(kRowHeight * 5 + 220);

    wake_countdown_timer_ = new QTimer(this);
    wake_countdown_timer_->setInterval(1000);
    connect(wake_countdown_timer_, &QTimer::timeout, this,
            &PreScanChecklistDialog::onWakeCountdownTick);
}

void PreScanChecklistDialog::applyStyle() {
    const auto t = uiThemeTokens(dark_mode_);
    const QString accent = dark_mode_ ? QStringLiteral("#10B981")
                                      : QStringLiteral("#155DFC");
    const QString accent_hover = dark_mode_ ? QStringLiteral("#34D399")
                                            : QStringLiteral("#1D4ED8");
    const QString row_hover = dark_mode_ ? QStringLiteral("#1F2937")
                                         : QStringLiteral("#F1F5F9");
    const QString disabled_bg = dark_mode_ ? QStringLiteral("#1F2937")
                                           : QStringLiteral("#E5E7EB");
    const QString disabled_fg = dark_mode_ ? QStringLiteral("#9CA3AF")
                                           : QStringLiteral("#6B7280");

    setStyleSheet(QString(R"(
        #PreScanChecklistContainer {
            background-color: %1;
            border: 1px solid %2;
            border-radius: 12px;
        }
        #PreScanChecklistHeader {
            background-color: %1;
            border-top-left-radius: 12px;
            border-top-right-radius: 12px;
            border-bottom: 1px solid %2;
        }
        #PreScanChecklistTitle {
            background: transparent;
            color: %3;
            font-family: 'Arimo';
            font-size: 22px;
            font-weight: 700;
        }
        #PreScanChecklistSubtitle {
            background: transparent;
            color: %4;
            font-family: 'Arimo';
            font-size: 13px;
            font-weight: 400;
        }
        #PreScanChecklistList {
            background-color: %1;
            border: none;
            outline: none;
        }
        #PreScanChecklistList::item {
            background: transparent;
            border-bottom: 1px solid %2;
            padding: 0px;
        }
        #PreScanChecklistList::item:hover {
            background-color: %5;
        }
        #PreScanChecklistList::item:selected {
            background-color: %5;
            color: %3;
        }
        #PreScanChecklistRowText {
            background: transparent;
            color: %3;
            font-family: 'Arimo';
            font-size: 14px;
            font-weight: 500;
        }
        #PreScanChecklistWakeBtn {
            background-color: %6;
            color: #FFFFFF;
            border: none;
            border-radius: 6px;
            padding: 0px 16px;
            font-family: 'Arimo';
            font-size: 13px;
            font-weight: 600;
        }
        #PreScanChecklistWakeBtn:hover {
            background-color: %7;
        }
        #PreScanChecklistWakeBtn:disabled {
            background-color: %8;
            color: %9;
        }
        #PreScanChecklistFooter {
            background-color: %1;
            border-bottom-left-radius: 12px;
            border-bottom-right-radius: 12px;
            border-top: 1px solid %2;
        }
        #PreScanChecklistConfirmBtn {
            background-color: %6;
            color: #FFFFFF;
            border: none;
            border-radius: 8px;
            padding: 0px 24px;
            font-family: 'Arimo';
            font-size: 14px;
            font-weight: 600;
        }
        #PreScanChecklistConfirmBtn:hover {
            background-color: %7;
        }
        #PreScanChecklistConfirmBtn:disabled {
            background-color: %8;
            color: %9;
        }
    )")
                      .arg(t.card_bg, t.border, t.text, t.muted, row_hover,
                           accent, accent_hover, disabled_bg, disabled_fg));
}

bool PreScanChecklistDialog::allRowsTicked() const {
    return list_ && list_->selectedItems().size() == list_->count();
}

void PreScanChecklistDialog::onRowSelectionChanged() {
    if (btn_confirm_) {
        btn_confirm_->setEnabled(allRowsTicked());
    }
    // Force every row widget to repaint. Selection can change without a
    // mousePressEvent (keyboard arrow + Space, or programmatic
    // setSelected) and ChecklistRowWidget has no other way to learn
    // about it. Repainting unconditionally is cheap (5 widgets, small
    // area) and keeps the check state in sync with the model.
    if (list_) {
        for (int i = 0; i < list_->count(); ++i) {
            if (auto* w = list_->itemWidget(list_->item(i))) {
                w->update();
            }
        }
    }
}

void PreScanChecklistDialog::onConfirmClicked() {
    if (!allRowsTicked()) {
        return;
    }
    accept();
}

void PreScanChecklistDialog::onWakeClicked() {
    emit wakeGprRequested();
    if (!btn_wake_ || !wake_countdown_timer_) {
        return;
    }
    wake_countdown_remaining_s_ = kWakeCountdownStartS;
    btn_wake_->setEnabled(false);
    btn_wake_->setText(QStringLiteral("Wake sent (%1)")
                           .arg(wake_countdown_remaining_s_));
    wake_countdown_timer_->start();
}

void PreScanChecklistDialog::onWakeCountdownTick() {
    if (!btn_wake_ || !wake_countdown_timer_) {
        return;
    }
    --wake_countdown_remaining_s_;
    if (wake_countdown_remaining_s_ <= 0) {
        wake_countdown_timer_->stop();
        btn_wake_->setText(QStringLiteral("Wake GPR"));
        btn_wake_->setEnabled(true);
        return;
    }
    btn_wake_->setText(QStringLiteral("Wake sent (%1)")
                           .arg(wake_countdown_remaining_s_));
}

void PreScanChecklistDialog::keyPressEvent(QKeyEvent* event) {
    // Swallow Esc — the dialog is a hard preflight gate. Operator must
    // tick all rows and press Confirm; there is no abort path.
    if (event->key() == Qt::Key_Escape) {
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

void PreScanChecklistDialog::closeEvent(QCloseEvent* event) {
    // Block window-manager close (Alt+F4 / system close menu). Same
    // rationale as keyPressEvent — the only exit is Confirm.
    event->ignore();
}

void PreScanChecklistDialog::reject() {
    // No-op. QDialog::reject() is invoked by Esc fallback paths and by
    // some style/platform code; we treat the dialog as Confirm-only.
    // accept() is reachable through onConfirmClicked() and remains the
    // sole way to dismiss this dialog.
}

void PreScanChecklistDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    // Hand keyboard focus to the list so Space toggles the highlighted
    // row and arrow keys move between rows. Confirm has setDefault(true)
    // so Enter still triggers it once enabled.
    if (list_) {
        list_->setFocus(Qt::OtherFocusReason);
        if (list_->count() > 0) {
            list_->setCurrentRow(0);
        }
    }
}

}  // namespace f2c_cpp
