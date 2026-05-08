#include "exploration_screen.hpp"

#include "components/auto_hide_scroll_bar.hpp"

#include "coverage_gui.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <QEvent>
#include <QFile>
#include <QFocusEvent>
#include <QFrame>
#include <QFont>
#include <QFontMetrics>
#include <QGraphicsBlurEffect>
#include <QGridLayout>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPlainTextEdit>
#include <QPolygonF>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QSize>
#include <QSizePolicy>
#include <QStackedLayout>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleOption>
#include <QTextCursor>
#include <QTimer>
#include <QDateTime>
#include <QElapsedTimer>
#include <QVBoxLayout>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QSvgRenderer>
#else
#include <QtSvg/QSvgRenderer>
#endif

namespace f2c_cpp {

namespace {

constexpr int kTopStatusItemHeight = 20;
constexpr int kTopStatusBatteryMinWidth = 52;
constexpr int kTopStatusSignalMinWidth = 69;
constexpr int kTopStatusLockMinWidth = 69;
constexpr int kTopStatusMotorsChipMinWidth = 96;
constexpr int kTopStatusMotorsChipHeight = 20;
constexpr int kTopStatusMotorsChipHorizontalPadding = 9;
constexpr int kTopStatusMotorsChipSpacing = 6;
constexpr int kTopStatusWindowControlsReservedWidth = 184;

// Width budget for the bottom-bar action buttons (Start Scan / Start
// Planning). Both buttons share the same inner layout: contentsMargins
// (24, 0, 12, 0), 6 px spacing, items = [icon, label, stretch]. The
// stretch is a real layout item so QHBoxLayout reserves its inter-item
// spacing (6 px) on both sides of the label, not just before it. Total
// non-text reservation = 24 + 20 + 6 + 6 + 12 = 68 px.
constexpr int kExplActionButtonChrome = 24 + 20 + 6 + 6 + 12;
// Slack on top of QFontMetrics::horizontalAdvance() to cover bold-glyph
// ink overhang and Qt's sub-pixel-to-integer layout rounding. Tuned
// empirically against Arimo Bold 16 px.
constexpr int kExplActionButtonSafetyPad = 8;
// Floor preserved for the planning button so the single-text "Start Planning"
// CTA matches its Figma minimum even though the chrome+advance math
// produces a slightly smaller value.
constexpr int kExplStartPlanningMinWidth = 185;

QByteArray stripPngIccpChunk(const QByteArray& png_data) {
    static const unsigned char kPngSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (png_data.size() < 8) {
        return png_data;
    }
    if (std::memcmp(png_data.constData(), kPngSig, 8) != 0) {
        return png_data;
    }

    QByteArray out;
    out.reserve(png_data.size());
    out.append(png_data.constData(), 8);

    int pos = 8;
    bool removed_iccp = false;
    while (pos + 12 <= png_data.size()) {
        const unsigned char* b = reinterpret_cast<const unsigned char*>(png_data.constData() + pos);
        const uint32_t len = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) |
                             uint32_t(b[3]);
        const int chunk_total = 12 + static_cast<int>(len);
        if (len > static_cast<uint32_t>(png_data.size()) || pos + chunk_total > png_data.size()) {
            return png_data;
        }

        const char* type = png_data.constData() + pos + 4;
        const bool is_iccp = std::memcmp(type, "iCCP", 4) == 0;
        if (!is_iccp) {
            out.append(png_data.constData() + pos, chunk_total);
        } else {
            removed_iccp = true;
        }

        pos += chunk_total;
        if (std::memcmp(type, "IEND", 4) == 0) {
            break;
        }
    }

    return removed_iccp ? out : png_data;
}

QPixmap loadSvgPixmap(const QString& resource_path,
                      int w,
                      int h,
                      const QString& color = QString()) {
    QFile f(resource_path);
    if (!f.open(QIODevice::ReadOnly)) {
        return QPixmap();
    }
    QString svg = QString::fromUtf8(f.readAll());
    f.close();

    if (!color.isEmpty()) {
        svg.replace(QStringLiteral("currentColor"), color);
    }

    static const QRegularExpression kFigmaVarColorPattern(
        QStringLiteral(R"(var\(--(?:fill|stroke)-\d+,\s*(#[0-9A-Fa-f]{3,8})\s*\))"));
    QString resolved_svg;
    resolved_svg.reserve(svg.size());
    int cursor = 0;
    QRegularExpressionMatchIterator it = kFigmaVarColorPattern.globalMatch(svg);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const int start = match.capturedStart(0);
        const int end = match.capturedEnd(0);
        if (start < 0 || end < start) {
            continue;
        }
        resolved_svg += svg.mid(cursor, start - cursor);
        resolved_svg += color.isEmpty() ? match.captured(1) : color;
        cursor = end;
    }
    if (cursor > 0) {
        resolved_svg += svg.mid(cursor);
        svg = resolved_svg;
    }

    QSvgRenderer renderer(svg.toUtf8());
    if (!renderer.isValid()) {
        return QPixmap();
    }

    QPixmap pix(w, h);
    pix.fill(Qt::transparent);
    QPainter painter(&pix);
    renderer.render(&painter);
    return pix;
}

QPixmap loadAssetPixmap(const QString& resource_path) {
    QFile f(resource_path);
    if (!f.open(QIODevice::ReadOnly)) {
        return QPixmap();
    }
    const QByteArray data = f.readAll();
    f.close();

    QPixmap pix;
    // Prefer decoding embedded data URLs ourselves, because Qt's SVG path can
    // occasionally render these as black frames even when load() succeeds.
    const QString text = QString::fromUtf8(data);
    const QRegularExpression re(R"(data:image\/[a-zA-Z0-9.+-]+;base64,([A-Za-z0-9+/=]+))",
                                QRegularExpression::CaseInsensitiveOption);
    const auto match = re.match(text);
    if (match.hasMatch()) {
        const QByteArray decoded = QByteArray::fromBase64(match.captured(1).toUtf8());
        const QByteArray decoded_clean = stripPngIccpChunk(decoded);
        if (pix.loadFromData(decoded_clean)) {
            return pix;
        }
    }

    const QByteArray data_clean = stripPngIccpChunk(data);
    if (pix.loadFromData(data_clean)) {
        return pix;
    }
    if (pix.load(resource_path)) {
        return pix;
    }
    return QPixmap();
}

QLabel* makeSvgIcon(const QString& resource_path,
                    int w,
                    int h,
                    QWidget* parent,
                    const QString& object_name = QString()) {
    auto* icon_lbl = new QLabel(parent);
    if (!object_name.isEmpty()) {
        icon_lbl->setObjectName(object_name);
    }
    icon_lbl->setFixedSize(w, h);
    icon_lbl->setAlignment(Qt::AlignCenter);
    const QPixmap icon_pix = loadSvgPixmap(resource_path, w, h);
    if (!icon_pix.isNull()) {
        icon_lbl->setPixmap(icon_pix);
    }
    return icon_lbl;
}

QLabel* makePlannerStatusIconLabel(QWidget* parent,
                                   const QString& resource_path,
                                   int size,
                                   const QString& color = QString()) {
    auto* label = new QLabel(parent);
    label->setFixedSize(size, size);
    label->setAlignment(Qt::AlignCenter);
    label->setAttribute(Qt::WA_TranslucentBackground, true);
    label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    label->setStyleSheet(QStringLiteral("background: transparent;"));
    label->setPixmap(loadSvgPixmap(resource_path, size, size, color));
    return label;
}

QLabel* makePlannerStatusTextLabel(QWidget* parent,
                                   const QString& text,
                                   const QString& style,
                                   Qt::Alignment alignment = Qt::AlignLeft | Qt::AlignVCenter) {
    auto* label = new QLabel(text, parent);
    label->setAlignment(alignment);
    label->setAttribute(Qt::WA_TranslucentBackground, true);
    label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    label->setStyleSheet(style + QStringLiteral(" background: transparent;"));
    return label;
}

QWidget* makePlannerStatusItem(QWidget* parent,
                               const QString& resource_path,
                               int icon_size,
                               const QString& text,
                               int minimum_width,
                               const QString& text_style,
                               const QString& color = QString(),
                               QLabel** out_label = nullptr) {
    auto* item = new QWidget(parent);
    item->setFixedHeight(kTopStatusItemHeight);
    item->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    if (minimum_width > 0) {
        item->setMinimumWidth(minimum_width);
    }

    auto* layout = new QHBoxLayout(item);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    layout->addWidget(makePlannerStatusIconLabel(item, resource_path, icon_size, color),
                      0,
                      Qt::AlignVCenter);
    auto* label = makePlannerStatusTextLabel(item, text, text_style);
    if (out_label) {
        *out_label = label;
    }
    layout->addWidget(label, 0, Qt::AlignVCenter);
    layout->addStretch(1);
    return item;
}

QLabel* makeFeedImage(const QString& resource_path, QWidget* parent, const QString& object_name) {
    auto* image = new QLabel(parent);
    image->setObjectName(object_name);
    image->setScaledContents(true);
    const QPixmap pix = loadAssetPixmap(resource_path);
    if (!pix.isNull()) {
        image->setPixmap(pix);
    }
    return image;
}

QWidget* makeHeadingRow(const QString& icon_alias, const QString& title, QWidget* parent) {
    auto* row = new QWidget(parent);
    row->setObjectName("ExplCardHeadingRow");
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    layout->addWidget(makeSvgIcon(icon_alias, 16, 16, row, "ExplCardHeadingIcon"), 0, Qt::AlignVCenter);

    auto* title_lbl = new QLabel(title, row);
    title_lbl->setObjectName("ExplCardHeadingTitle");
    layout->addWidget(title_lbl, 1, Qt::AlignVCenter);

    return row;
}

QWidget* makeMetricRow(const QString& label,
                       const QString& value,
                       const QString& value_object_name,
                       QWidget* parent,
                       QLabel** value_widget_out = nullptr) {
    auto* row = new QWidget(parent);
    row->setObjectName("ExplMetricRow");
    auto* layout = new QVBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* label_widget = new QLabel(label, row);
    label_widget->setObjectName("ExplMetricLabel");
    layout->addWidget(label_widget);

    auto* value_widget = new QLabel(value, row);
    value_widget->setObjectName(value_object_name);
    value_widget->setWordWrap(true);
    if (value_widget_out) {
        *value_widget_out = value_widget;
    }
    layout->addWidget(value_widget);

    return row;
}

QWidget* makeStatusItem(const QString& icon_alias,
                        int icon_size,
                        const QString& text,
                        QWidget* parent,
                        QLabel** text_widget_out = nullptr) {
    auto* item = new QWidget(parent);
    item->setObjectName("ExplTopStatusItem");
    auto* layout = new QHBoxLayout(item);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    layout->addWidget(makeSvgIcon(icon_alias, icon_size, icon_size, item, "ExplTopStatusIcon"), 0, Qt::AlignVCenter);

    auto* text_lbl = new QLabel(text, item);
    text_lbl->setObjectName("ExplTopStatusText");
    if (text_widget_out) {
        *text_widget_out = text_lbl;
    }
    layout->addWidget(text_lbl, 0, Qt::AlignVCenter);
    return item;
}

QString wrapTextToTwoCenteredLines(const QString& text, const QFontMetrics& metrics, int max_width) {
    const QString simplified = text.simplified();
    if (simplified.isEmpty()) {
        return QString();
    }
    if (metrics.horizontalAdvance(simplified) <= max_width) {
        return simplified;
    }

    const QStringList words = simplified.split(' ', Qt::SkipEmptyParts);
    if (words.isEmpty()) {
        return metrics.elidedText(simplified, Qt::ElideRight, max_width);
    }

    QString first_line;
    int split_index = 0;
    for (int i = 0; i < words.size(); ++i) {
        const QString candidate =
            first_line.isEmpty() ? words.at(i) : QString("%1 %2").arg(first_line, words.at(i));
        if (!first_line.isEmpty() && metrics.horizontalAdvance(candidate) > max_width) {
            break;
        }
        if (first_line.isEmpty() && metrics.horizontalAdvance(candidate) > max_width) {
            first_line = metrics.elidedText(words.at(i), Qt::ElideRight, max_width);
            split_index = i + 1;
            break;
        }
        first_line = candidate;
        split_index = i + 1;
    }

    if (split_index >= words.size()) {
        return first_line;
    }

    const QString second_source = words.mid(split_index).join(' ');
    const QString second_line = metrics.elidedText(second_source, Qt::ElideRight, max_width);
    return second_line.isEmpty() ? first_line : QString("%1\n%2").arg(first_line, second_line);
}

const std::array<qreal, 11> kLoaderKeyframes = {
    1.0 / 11.0, 2.0 / 11.0, 3.0 / 11.0, 4.0 / 11.0, 5.0 / 11.0, 6.0 / 11.0,
    7.0 / 11.0, 8.0 / 11.0, 9.0 / 11.0, 10.0 / 11.0, 1.0};

const std::array<std::array<QPoint, 11>, 9> kLoaderTranslations = {{
    {{{-26, 0}, {0, 0}, {0, 0}, {26, 0}, {26, 26}, {26, 26},
      {26, 26}, {26, 0}, {0, 0}, {-26, 0}, {0, 0}}},
    {{{0, 0}, {26, 0}, {0, 0}, {26, 0}, {26, 26}, {26, 26},
      {26, 26}, {26, 26}, {0, 26}, {0, 26}, {0, 0}}},
    {{{-26, 0}, {-26, 0}, {0, 0}, {-26, 0}, {-26, 0}, {-26, 0},
      {-26, 0}, {-26, 0}, {-26, -26}, {0, -26}, {0, 0}}},
    {{{-26, 0}, {-26, 0}, {-26, -26}, {0, -26}, {0, 0}, {0, -26},
      {0, -26}, {0, -26}, {-26, -26}, {-26, 0}, {0, 0}}},
    {{{0, 0}, {0, 0}, {0, 0}, {26, 0}, {26, 0}, {26, 0},
      {26, 0}, {26, 0}, {26, -26}, {0, -26}, {0, 0}}},
    {{{0, 0}, {-26, 0}, {-26, 0}, {0, 0}, {0, 0}, {0, 0},
      {0, 0}, {0, 26}, {-26, 26}, {-26, 0}, {0, 0}}},
    {{{26, 0}, {26, 0}, {26, 0}, {0, 0}, {0, -26}, {26, -26},
      {0, -26}, {0, -26}, {0, 0}, {26, 0}, {0, 0}}},
    {{{0, 0}, {-26, 0}, {-26, -26}, {0, -26}, {0, -26}, {0, -26},
      {0, -26}, {0, -26}, {26, -26}, {26, 0}, {0, 0}}},
    {{{-26, 0}, {-26, 0}, {0, 0}, {-26, 0}, {0, 0}, {0, 0},
      {-26, 0}, {-26, 0}, {-52, 0}, {-26, 0}, {0, 0}}},
}};

QPoint loaderBasePositionForIndex(int index) {
    const int row = index / 3;
    const int col = index % 3;
    QPoint pos(col * 26, row * 26);
    if (index == 0 || index == 3) {
        pos.rx() += 26;
    } else if (index == 2) {
        pos.ry() += 52;
    }
    return pos;
}

}  // namespace

class ExplorationLoadingOverlayWidget : public QWidget {
public:
    explicit ExplorationLoadingOverlayWidget(QWidget* parent = nullptr)
        : QWidget(parent) {
        animation_timer_.setInterval(16);
        connect(&animation_timer_, &QTimer::timeout, this, [this]() { update(); });
    }

    void setDarkMode(bool dark_mode) {
        if (dark_mode_ == dark_mode) {
            return;
        }
        dark_mode_ = dark_mode;
        update();
    }

    void setStatusText(const QString& status_text) {
        const QString normalized =
            status_text.trimmed().isEmpty() ? QStringLiteral("Launching pipeline...")
                                            : status_text.trimmed();
        if (status_text_ == normalized) {
            return;
        }
        status_text_ = normalized;
        update();
    }

    void setPercent(int percent) {
        const int clamped = std::max(0, std::min(100, percent));
        if (percent_ == clamped) {
            return;
        }
        percent_ = clamped;
        update();
    }

    void startAnimation() {
        if (running_) {
            return;
        }
        animation_clock_.restart();
        animation_timer_.start();
        running_ = true;
        update();
    }

    void stopAnimation() {
        if (!running_) {
            return;
        }
        animation_timer_.stop();
        running_ = false;
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);

        QStyleOption option;
        option.initFrom(this);
        style()->drawPrimitive(QStyle::PE_Widget, &option, &painter, this);

        drawLoader(painter);
        drawStatusText(painter);
        drawPercentText(painter);
    }

private:
    QRectF contentFrameRect() const {
        constexpr qreal kContentWidth = 248.0;
        constexpr qreal kContentHeight = 180.0;
        return QRectF((width() - kContentWidth) * 0.5,
                      (height() - kContentHeight) * 0.5,
                      kContentWidth,
                      kContentHeight);
    }

    QRectF loaderRect() const {
        const QRectF frame = contentFrameRect();
        return QRectF(frame.center().x() - 36.0, frame.top() + 16.0, 72.0, 72.0);
    }

    QRectF statusRect() const {
        const QRectF frame = contentFrameRect();
        return QRectF(frame.left() + 20.0, frame.top() + 96.0, frame.width() - 40.0, 50.0);
    }

    QRectF percentRect() const {
        const QRectF frame = contentFrameRect();
        return QRectF(frame.left() + 20.0, frame.top() + 152.0, frame.width() - 40.0, 22.0);
    }

    QColor textShadowColor() const {
        return dark_mode_ ? QColor(0, 0, 0, 88) : QColor(0, 0, 0, 108);
    }

    QColor loaderShadowColor() const {
        return dark_mode_ ? QColor(0, 0, 0, 76) : QColor(0, 0, 0, 96);
    }

    QPointF loaderOffsetAt(int index, qreal progress) const {
        if (progress <= 0.0) {
            return QPointF(0.0, 0.0);
        }

        qreal prev_t = 0.0;
        QPoint prev(0, 0);
        for (int step = 0; step < static_cast<int>(kLoaderKeyframes.size()); ++step) {
            const qreal next_t = kLoaderKeyframes[step];
            const QPoint next = kLoaderTranslations[index][step];
            if (progress <= next_t) {
                const qreal span = std::max<qreal>(1e-6, next_t - prev_t);
                const qreal local = std::clamp((progress - prev_t) / span, 0.0, 1.0);
                return QPointF(prev.x() + ((next.x() - prev.x()) * local),
                               prev.y() + ((next.y() - prev.y()) * local));
            }
            prev_t = next_t;
            prev = next;
        }

        return QPointF(prev);
    }

    void drawLoaderPass(QPainter& painter,
                        const QRectF& loader_rect,
                        qreal progress,
                        const QPointF& offset,
                        const QColor& color) const {
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        for (int i = 0; i < 9; ++i) {
            const QPoint base = loaderBasePositionForIndex(i);
            const QPointF delta = loaderOffsetAt(i, progress);
            const QRectF box_rect(loader_rect.left() + base.x() + delta.x() + offset.x(),
                                  loader_rect.top() + base.y() + delta.y() + offset.y(),
                                  20.0,
                                  20.0);
            painter.drawRoundedRect(box_rect, 2.0, 2.0);
        }
        painter.restore();
    }

    void drawTextWithShadow(QPainter& painter,
                            const QRectF& rect,
                            const QString& text,
                            const QFont& font,
                            const QColor& text_color,
                            const QColor& shadow_color,
                            const QPointF& shadow_offset) const {
        constexpr int kTextFlags = Qt::AlignHCenter | Qt::AlignVCenter | Qt::TextDontClip;

        painter.save();
        painter.setFont(font);
        painter.setPen(shadow_color);
        painter.drawText(rect.translated(shadow_offset), kTextFlags, text);
        painter.setPen(text_color);
        painter.drawText(rect, kTextFlags, text);
        painter.restore();
    }

    void drawLoader(QPainter& painter) const {
        const QRectF loader_rect = loaderRect();
        const qreal progress =
            running_ ? std::fmod(static_cast<qreal>(animation_clock_.elapsed()), 4000.0) / 4000.0 : 0.0;

        drawLoaderPass(painter, loader_rect, progress, QPointF(0.0, 2.0), loaderShadowColor());
        drawLoaderPass(painter, loader_rect, progress, QPointF(0.0, 0.0), QColor("#FFFFFF"));
    }

    void drawStatusText(QPainter& painter) const {
        QFont status_font(QStringLiteral("Arimo"));
        status_font.setPixelSize(17);
        status_font.setBold(true);

        const QFontMetrics metrics(status_font);
        const QString wrapped =
            wrapTextToTwoCenteredLines(status_text_, metrics, static_cast<int>(statusRect().width()));
        drawTextWithShadow(
            painter, statusRect(), wrapped, status_font, QColor("#FFFFFF"), textShadowColor(), QPointF(0.0, 2.0));
    }

    void drawPercentText(QPainter& painter) const {
        QFont percent_font(QStringLiteral("Liberation Mono"));
        percent_font.setPixelSize(15);
        drawTextWithShadow(painter,
                           percentRect(),
                           QString("%1%").arg(percent_),
                           percent_font,
                           QColor(255, 255, 255, 235),
                           textShadowColor(),
                           QPointF(0.0, 2.0));
    }

    QTimer animation_timer_;
    QElapsedTimer animation_clock_;
    bool dark_mode_ = false;
    bool running_ = false;
    int percent_ = 0;
    QString status_text_ = QStringLiteral("Launching pipeline...");
};

class ExplorationNavMapWidget : public QWidget {
public:
    explicit ExplorationNavMapWidget(QWidget* parent = nullptr)
        : QWidget(parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setAttribute(Qt::WA_StyledBackground, false);
    }

    void setDarkMode(bool dark_mode) {
        if (dark_mode_ == dark_mode) {
            return;
        }
        dark_mode_ = dark_mode;
        cache_dirty_ = true;
        update();
    }

    void resetToEmpty() {
        if (!has_grid_ && !has_pose_ && !stale_) {
            return;
        }
        packed_grid_.clear();
        has_grid_ = false;
        has_pose_ = false;
        stale_ = false;
        yaw_rad_ = 0.0;
        cache_dirty_ = true;
        update();
    }

    void setPackedGrid(const QByteArray& packed_grid) {
        if (packed_grid.size() != kPackedGridBytes) {
            return;
        }
        if (has_grid_ && packed_grid_ == packed_grid) {
            return;
        }
        packed_grid_ = packed_grid;
        has_grid_ = true;
        cache_dirty_ = true;
        update();
    }

    void setYawRadians(double yaw_rad) {
        if (has_pose_ && std::abs(yaw_rad_ - yaw_rad) < 1e-3) {
            return;
        }
        yaw_rad_ = yaw_rad;
        has_pose_ = true;
        update();
    }

    void setStale(bool stale) {
        if (stale_ == stale) {
            return;
        }
        stale_ = stale;
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        ensureRasterCache();
        if (!raster_cache_.isNull()) {
            painter.drawPixmap(0, 0, raster_cache_);
        }

        const QRectF map_frame = mapFrameRect();
        const QRectF grid_rect = gridRect(map_frame);
        if (stale_ && has_grid_) {
            painter.save();
            painter.setPen(Qt::NoPen);
            painter.setBrush(dark_mode_ ? QColor(2, 6, 23, 132) : QColor(255, 255, 255, 120));
            painter.drawRoundedRect(map_frame, 14.0, 14.0);
            painter.restore();

            const QRectF chip_rect(map_frame.right() - 62.0, map_frame.top() + 10.0, 52.0, 22.0);
            painter.save();
            painter.setPen(Qt::NoPen);
            painter.setBrush(dark_mode_ ? QColor(15, 23, 42, 210) : QColor(226, 232, 240, 235));
            painter.drawRoundedRect(chip_rect, 6.0, 6.0);
            painter.setPen(dark_mode_ ? QColor("#F8FAFC") : QColor("#0F172A"));
            QFont chip_font("Arimo");
            chip_font.setPixelSize(11);
            chip_font.setBold(true);
            painter.setFont(chip_font);
            painter.drawText(chip_rect, Qt::AlignCenter, "STALE");
            painter.restore();
        }

        if (has_grid_ && has_pose_) {
            drawRobotArrow(painter, grid_rect);
        }
    }

private:
    static constexpr int kGridRows = 48;
    static constexpr int kGridCols = 48;
    static constexpr int kPackedGridBytes = (kGridRows * kGridCols) / 8;

    QRectF mapFrameRect() const {
        return rect().adjusted(6, 6, -6, -6);
    }

    QRectF gridRect(const QRectF& frame_rect) const {
        const qreal side = std::max<qreal>(0.0, std::min(frame_rect.width(), frame_rect.height()) - 6.0);
        return QRectF(frame_rect.center().x() - (side * 0.5),
                      frame_rect.center().y() - (side * 0.5),
                      side,
                      side);
    }

    bool cellOccupied(int row, int col) const {
        if (!has_grid_ || packed_grid_.size() != kPackedGridBytes) {
            return false;
        }
        const int bit_index = (row * kGridCols) + col;
        const uint8_t byte =
            static_cast<uint8_t>(packed_grid_.at(bit_index >> 3));
        return (byte & static_cast<uint8_t>(1u << (bit_index & 7))) != 0;
    }

    void ensureRasterCache() {
        if (!cache_dirty_ && raster_cache_.size() == size()) {
            return;
        }

        if (width() <= 0 || height() <= 0) {
            raster_cache_ = QPixmap();
            return;
        }

        raster_cache_ = QPixmap(size());
        raster_cache_.fill(Qt::transparent);

        QPainter painter(&raster_cache_);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QRectF frame_rect = mapFrameRect();
        const QRectF grid_rect = gridRect(frame_rect);
        const QColor panel_bg = dark_mode_ ? QColor("#030712") : QColor("#E5E7EB");
        const QColor panel_border = dark_mode_ ? QColor(71, 85, 105, 90) : QColor(148, 163, 184, 120);
        const QColor grid_line = dark_mode_ ? QColor(148, 163, 184, 28) : QColor(100, 116, 139, 42);
        const QColor axis_line = dark_mode_ ? QColor(148, 163, 184, 52) : QColor(100, 116, 139, 68);
        const QColor occupied_fill = dark_mode_ ? QColor(20, 184, 166, 216) : QColor(5, 150, 105, 214);

        painter.setPen(Qt::NoPen);
        painter.setBrush(panel_bg);
        painter.drawRoundedRect(frame_rect, 14.0, 14.0);

        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(panel_border, 1.0));
        painter.drawRoundedRect(frame_rect, 14.0, 14.0);

        if (grid_rect.width() <= 0.0 || grid_rect.height() <= 0.0) {
            cache_dirty_ = false;
            return;
        }

        const qreal cell_w = grid_rect.width() / static_cast<qreal>(kGridCols);
        const qreal cell_h = grid_rect.height() / static_cast<qreal>(kGridRows);

        painter.setPen(QPen(grid_line, 1.0));
        for (int col = 0; col <= kGridCols; ++col) {
            const qreal x = grid_rect.left() + (cell_w * static_cast<qreal>(col));
            painter.drawLine(QPointF(x, grid_rect.top()), QPointF(x, grid_rect.bottom()));
        }
        for (int row = 0; row <= kGridRows; ++row) {
            const qreal y = grid_rect.top() + (cell_h * static_cast<qreal>(row));
            painter.drawLine(QPointF(grid_rect.left(), y), QPointF(grid_rect.right(), y));
        }

        painter.setPen(QPen(axis_line, 1.2));
        painter.drawLine(QPointF(grid_rect.center().x(), grid_rect.top()),
                         QPointF(grid_rect.center().x(), grid_rect.bottom()));
        painter.drawLine(QPointF(grid_rect.left(), grid_rect.center().y()),
                         QPointF(grid_rect.right(), grid_rect.center().y()));

        if (has_grid_) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(occupied_fill);
            for (int row = 0; row < kGridRows; ++row) {
                for (int col = 0; col < kGridCols; ++col) {
                    if (!cellOccupied(row, col)) {
                        continue;
                    }
                    const QRectF cell_rect(grid_rect.left() + (cell_w * static_cast<qreal>(col)),
                                           grid_rect.top() + (cell_h * static_cast<qreal>(row)),
                                           cell_w,
                                           cell_h);
                    painter.drawRect(cell_rect);
                }
            }
        }

        cache_dirty_ = false;
    }

    void drawRobotArrow(QPainter& painter, const QRectF& grid_rect) const {
        if (grid_rect.isEmpty()) {
            return;
        }

        const QPointF center = grid_rect.center();
        const qreal arrow_len = std::clamp(grid_rect.width() * 0.065, 12.0, 20.0);
        const qreal arrow_half_width = arrow_len * 0.55;
        const QPointF direction(-std::sin(yaw_rad_), -std::cos(yaw_rad_));
        const QPointF perpendicular(-direction.y(), direction.x());

        const QPointF tip = center + QPointF(direction.x() * arrow_len, direction.y() * arrow_len);
        const QPointF base_center =
            center - QPointF(direction.x() * (arrow_len * 0.70), direction.y() * (arrow_len * 0.70));
        const QPointF left =
            base_center + QPointF(perpendicular.x() * arrow_half_width, perpendicular.y() * arrow_half_width);
        const QPointF right =
            base_center - QPointF(perpendicular.x() * arrow_half_width, perpendicular.y() * arrow_half_width);

        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(dark_mode_ ? QColor("#E2E8F0") : QColor("#0F172A"), 1.2));
        painter.setBrush(dark_mode_ ? QColor("#F8FAFC") : QColor("#111827"));
        QPolygonF triangle;
        triangle << tip << left << right;
        painter.drawPolygon(triangle);
        painter.setPen(Qt::NoPen);
        painter.setBrush(dark_mode_ ? QColor("#14B8A6") : QColor("#059669"));
        painter.drawEllipse(center, 2.8, 2.8);
        painter.restore();
    }

    bool dark_mode_ = false;
    bool has_grid_ = false;
    bool has_pose_ = false;
    bool stale_ = false;
    double yaw_rad_ = 0.0;
    bool cache_dirty_ = true;
    QByteArray packed_grid_;
    QPixmap raster_cache_;
};

class ExplorationThermalPixelWidget : public QWidget {
public:
    explicit ExplorationThermalPixelWidget(QWidget* parent = nullptr)
        : QWidget(parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setAttribute(Qt::WA_StyledBackground, false);
    }

    void setDarkMode(bool dark_mode) {
        if (dark_mode_ == dark_mode) {
            return;
        }
        dark_mode_ = dark_mode;
        cache_dirty_ = true;
        update();
    }

    void resetToEmpty() {
        if (!has_frame_) {
            return;
        }
        has_frame_ = false;
        packed_thumb_.clear();
        cache_dirty_ = true;
        update();
    }

    void setPackedThumbnail(const QByteArray& packed_thumb) {
        if (packed_thumb.size() != kPackedThumbBytes) {
            return;
        }
        if (has_frame_ && packed_thumb_ == packed_thumb) {
            return;
        }
        packed_thumb_ = packed_thumb;
        has_frame_ = true;
        cache_dirty_ = true;
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        ensureRasterCache();
        QPainter painter(this);
        if (!raster_cache_.isNull()) {
            painter.drawPixmap(0, 0, raster_cache_);
        }
    }

private:
    static constexpr int kThumbCols = 32;
    static constexpr int kThumbRows = 24;
    static constexpr int kPackedThumbBytes = (kThumbCols * kThumbRows) / 2;

    QRectF frameRect() const {
        return rect().adjusted(8, 8, -8, -8);
    }

    QRectF canvasRect(const QRectF& frame_rect) const {
        const qreal target_ratio = static_cast<qreal>(kThumbCols) / static_cast<qreal>(kThumbRows);
        qreal width = frame_rect.width() - 12.0;
        qreal height = width / target_ratio;
        if (height > frame_rect.height() - 12.0) {
            height = frame_rect.height() - 12.0;
            width = height * target_ratio;
        }
        width = std::max<qreal>(0.0, width);
        height = std::max<qreal>(0.0, height);
        return QRectF(frame_rect.center().x() - (width * 0.5),
                      frame_rect.center().y() - (height * 0.5),
                      width,
                      height);
    }

    uint8_t paletteIndexAt(int row, int col) const {
        if (!has_frame_ || packed_thumb_.size() != kPackedThumbBytes) {
            return 0;
        }
        const int pixel_index = (row * kThumbCols) + col;
        const uint8_t byte = static_cast<uint8_t>(packed_thumb_.at(pixel_index / 2));
        if ((pixel_index & 1) == 0) {
            return static_cast<uint8_t>((byte >> 4) & 0x0F);
        }
        return static_cast<uint8_t>(byte & 0x0F);
    }

    QColor thermalPaletteColor(uint8_t index) const {
        static const std::array<QColor, 16> kPalette = {
            QColor("#10061A"), QColor("#2D0A4B"), QColor("#4C1374"), QColor("#6C1F8E"),
            QColor("#8B277F"), QColor("#B02D62"), QColor("#D23A3A"), QColor("#E95B1F"),
            QColor("#F57C15"), QColor("#FB9A0E"), QColor("#FBB525"), QColor("#FAD245"),
            QColor("#F8E16F"), QColor("#F7ED9D"), QColor("#FBF4C7"), QColor("#FFF8E8"),
        };
        return kPalette[std::min<size_t>(index, kPalette.size() - 1)];
    }

    void ensureRasterCache() {
        if (!cache_dirty_ && raster_cache_.size() == size()) {
            return;
        }
        if (width() <= 0 || height() <= 0) {
            raster_cache_ = QPixmap();
            return;
        }

        raster_cache_ = QPixmap(size());
        raster_cache_.fill(Qt::transparent);

        QPainter painter(&raster_cache_);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QRectF frame_rect = frameRect();
        const QRectF canvas_rect = canvasRect(frame_rect);
        const QColor frame_bg = QColor("#030712");
        const QColor frame_border = dark_mode_ ? QColor(148, 163, 184, 80) : QColor(51, 65, 85, 96);
        const QColor canvas_bg = QColor("#070B14");
        const QColor canvas_border = dark_mode_ ? QColor(56, 189, 248, 52) : QColor(14, 165, 233, 42);

        painter.setPen(Qt::NoPen);
        painter.setBrush(frame_bg);
        painter.drawRoundedRect(frame_rect, 14.0, 14.0);

        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(frame_border, 1.0));
        painter.drawRoundedRect(frame_rect, 14.0, 14.0);

        painter.setPen(Qt::NoPen);
        painter.setBrush(canvas_bg);
        painter.drawRoundedRect(canvas_rect, 10.0, 10.0);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(canvas_border, 1.0));
        painter.drawRoundedRect(canvas_rect, 10.0, 10.0);

        if (!has_frame_ || canvas_rect.width() <= 0.0 || canvas_rect.height() <= 0.0) {
            cache_dirty_ = false;
            return;
        }

        const qreal cell_w = canvas_rect.width() / static_cast<qreal>(kThumbCols);
        const qreal cell_h = canvas_rect.height() / static_cast<qreal>(kThumbRows);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(Qt::NoPen);
        for (int row = 0; row < kThumbRows; ++row) {
            for (int col = 0; col < kThumbCols; ++col) {
                const QRectF cell_rect(canvas_rect.left() + (cell_w * static_cast<qreal>(col)),
                                       canvas_rect.top() + (cell_h * static_cast<qreal>(row)),
                                       std::ceil(cell_w + 0.25),
                                       std::ceil(cell_h + 0.25));
                painter.fillRect(cell_rect, thermalPaletteColor(paletteIndexAt(row, col)));
            }
        }

        cache_dirty_ = false;
    }

    bool dark_mode_ = false;
    bool has_frame_ = false;
    bool cache_dirty_ = true;
    QByteArray packed_thumb_;
    QPixmap raster_cache_;
};

ExplorationScreen::ExplorationScreen(QWidget* parent)
    : QWidget(parent) {
    setObjectName("ExplorationRoot");
    setAttribute(Qt::WA_StyledBackground, true);
    setAutoFillBackground(true);
    setFocusPolicy(Qt::StrongFocus);
    buildUi();
    applyStyle();
    mapping_lock_timer_ = new QTimer(this);
    mapping_lock_timer_->setInterval(1000);
    connect(mapping_lock_timer_, &QTimer::timeout, this, &ExplorationScreen::onMappingLockTick);
    teleop_publish_timer_ = new QTimer(this);
    teleop_publish_timer_->setInterval(100);
    connect(teleop_publish_timer_, &QTimer::timeout, this, &ExplorationScreen::onTeleopPublishTick);
    teleop_publish_timer_->start();

    setLaunchProgress(0, "Waiting for Start Scan");
    setLaunchDiagnostics("Press Start Scan to begin launch diagnostics.");
    setLaunchReady(false);
    resetMappingWorkflowUi();
    setPlanningEnabled(false);
    setLoadingOverlayVisible(false);
    setTopSignalState(top_signal_text_, top_signal_tone_);
    setTopLockChipState(top_lock_text_, top_lock_tone_);
    setTopMotorsChipState(top_motors_text_, top_motors_tone_);
    setTelemetrySpeedMps(0.0);
    setTelemetryPositionMeters(0.0, 0.0);
    setTelemetryAltitudeMeters(0.0);
    setTelemetryScanTimeSeconds(0);
    setFpvSpeedMps(0.0);
    setThermalHidden();
    resetNavigationMap();
    setHardwareStatus("Unknown",
                      ValueTone::Muted,
                      "No data",
                      ValueTone::Muted,
                      "No stream",
                      ValueTone::Muted,
                      "N/A",
                      ValueTone::Muted);
}

void ExplorationScreen::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateLoadingOverlayGeometry();
}

void ExplorationScreen::setDarkMode(bool dark_mode) {
    if (dark_mode_ == dark_mode) {
        return;
    }
    dark_mode_ = dark_mode;
    if (loading_overlay_) {
        loading_overlay_->setDarkMode(dark_mode_);
    }
    if (nav_map_widget_) {
        nav_map_widget_->setDarkMode(dark_mode_);
    }
    if (thermal_pixel_widget_) {
        thermal_pixel_widget_->setDarkMode(dark_mode_);
    }
    const auto auto_hide_bars = findChildren<AutoHideScrollBar*>();
    for (auto* bar : auto_hide_bars) {
        bar->setDarkMode(dark_mode_);
    }
    applyStyle();
}

void ExplorationScreen::applyToneToLabel(QLabel* label, ValueTone tone, bool emphasize) {
    if (!label) {
        return;
    }
    QString color = "#9F9FA9";
    switch (tone) {
        case ValueTone::Good:
            color = "#10B981";
            break;
        case ValueTone::Warning:
            color = "#F59E0B";
            break;
        case ValueTone::Muted:
            color = "#9F9FA9";
            break;
        case ValueTone::Error:
            color = "#EF4444";
            break;
    }
    int weight = emphasize ? 700 : 600;
    if (tone == ValueTone::Muted) {
        weight = 500;
    }
    label->setStyleSheet(QString("color: %1; font-size: 14px; line-height: 20px; font-weight: %2;")
                             .arg(color)
                             .arg(weight));
}

void ExplorationScreen::applyTopStatusToneToLabel(QLabel* label, ValueTone tone, bool emphasize) {
    if (!label) {
        return;
    }

    QString color = dark_mode_ ? QStringLiteral("#9F9FA9") : QStringLiteral("#6B7280");
    switch (tone) {
        case ValueTone::Good:
            color = QStringLiteral("#10B981");
            break;
        case ValueTone::Warning:
            color = QStringLiteral("#F59E0B");
            break;
        case ValueTone::Muted:
            break;
        case ValueTone::Error:
            color = QStringLiteral("#EF4444");
            break;
    }

    const int weight = emphasize ? 500 : 400;
    label->setStyleSheet(
        QStringLiteral("font-family: 'Arimo'; font-size: 14px; font-weight: %1; color: %2; "
                       "background: transparent;")
            .arg(weight)
            .arg(color));
}

void ExplorationScreen::setTopLockChipState(const QString& text, ValueTone tone) {
    top_lock_text_ = text;
    top_lock_tone_ = tone;
    if (!lbl_top_lock_chip_) {
        return;
    }
    lbl_top_lock_chip_->setText(text);
    applyTopStatusToneToLabel(lbl_top_lock_chip_, tone, true);

    QString icon_color = dark_mode_ ? QStringLiteral("#9F9FA9") : QStringLiteral("#6B7280");
    switch (tone) {
        case ValueTone::Good:
            icon_color = QStringLiteral("#10B981");
            break;
        case ValueTone::Warning:
            icon_color = QStringLiteral("#F59E0B");
            break;
        case ValueTone::Muted:
            break;
        case ValueTone::Error:
            icon_color = QStringLiteral("#EF4444");
            break;
    }
    if (QWidget* item = lbl_top_lock_chip_->parentWidget()) {
        const auto icons = item->findChildren<QLabel*>(QString(), Qt::FindDirectChildrenOnly);
        for (QLabel* icon_label : icons) {
            if (icon_label && icon_label != lbl_top_lock_chip_) {
                icon_label->setPixmap(loadSvgPixmap(
                    QStringLiteral(":/assets/missionplanner/lock.svg"), 16, 16, icon_color));
                break;
            }
        }
        item->updateGeometry();
        item->adjustSize();
        if (QWidget* bar = item->parentWidget()) {
            bar->updateGeometry();
            bar->adjustSize();
            if (QWidget* host = bar->parentWidget()) {
                host->updateGeometry();
                host->adjustSize();
            }
        }
    }
}

void ExplorationScreen::setTopSignalState(const QString& text, ValueTone tone) {
    top_signal_text_ = text;
    top_signal_tone_ = tone;
    if (!lbl_top_signal_) {
        return;
    }
    lbl_top_signal_->setText(text);
    applyTopStatusToneToLabel(lbl_top_signal_, tone, false);

    QString icon_color = dark_mode_ ? QStringLiteral("#9F9FA9") : QStringLiteral("#6B7280");
    switch (tone) {
        case ValueTone::Good:
            icon_color = QStringLiteral("#10B981");
            break;
        case ValueTone::Warning:
            icon_color = QStringLiteral("#F59E0B");
            break;
        case ValueTone::Muted:
            break;
        case ValueTone::Error:
            icon_color = QStringLiteral("#EF4444");
            break;
    }
    if (QWidget* item = lbl_top_signal_->parentWidget()) {
        const auto icons = item->findChildren<QLabel*>(QString(), Qt::FindDirectChildrenOnly);
        for (QLabel* icon_label : icons) {
            if (icon_label && icon_label != lbl_top_signal_) {
                icon_label->setPixmap(loadSvgPixmap(
                    QStringLiteral(":/assets/missionplanner/status_dot.svg"), 8, 8, icon_color));
                break;
            }
        }
        item->updateGeometry();
        item->adjustSize();
        if (QWidget* bar = item->parentWidget()) {
            bar->updateGeometry();
            bar->adjustSize();
            if (QWidget* host = bar->parentWidget()) {
                host->updateGeometry();
                host->adjustSize();
            }
        }
    }
}

void ExplorationScreen::updateTopMotorsChipGeometry() {
    if (!top_motors_chip_ || !lbl_top_motors_text_ || !lbl_top_motors_dot_) {
        return;
    }

    lbl_top_motors_text_->adjustSize();
    const int text_width = lbl_top_motors_text_->sizeHint().width();
    const int dot_width = std::max(0, lbl_top_motors_dot_->width());
    const int chip_width =
        std::max(kTopStatusMotorsChipMinWidth,
                 (2 * kTopStatusMotorsChipHorizontalPadding) + dot_width + kTopStatusMotorsChipSpacing +
                     text_width + 4);
    top_motors_chip_->setFixedSize(chip_width, kTopStatusMotorsChipHeight);
    top_motors_chip_->updateGeometry();
    if (QWidget* bar = top_motors_chip_->parentWidget()) {
        bar->updateGeometry();
        bar->adjustSize();
        if (QWidget* host = bar->parentWidget()) {
            host->updateGeometry();
            host->adjustSize();
        }
    }
}

void ExplorationScreen::setTopMotorsChipState(const QString& text, ValueTone tone) {
    top_motors_text_ = text;
    top_motors_tone_ = tone;
    if (!top_motors_chip_ || !lbl_top_motors_dot_ || !lbl_top_motors_text_) {
        return;
    }

    QString bg = dark_mode_ ? QStringLiteral("rgba(113,113,123,0.18)")
                            : QStringLiteral("rgba(100,116,139,0.10)");
    QString border = dark_mode_ ? QStringLiteral("rgba(113,113,123,0.28)")
                                : QStringLiteral("rgba(100,116,139,0.18)");
    QString text_color = dark_mode_ ? QStringLiteral("#71717B") : QStringLiteral("#6B7280");
    switch (tone) {
        case ValueTone::Good:
            bg = dark_mode_ ? QStringLiteral("rgba(0,188,125,0.12)")
                            : QStringLiteral("rgba(5,150,105,0.10)");
            border = dark_mode_ ? QStringLiteral("rgba(0,188,125,0.24)")
                                : QStringLiteral("rgba(5,150,105,0.18)");
            text_color = dark_mode_ ? QStringLiteral("#00D492") : QStringLiteral("#059669");
            break;
        case ValueTone::Warning:
            bg = dark_mode_ ? QStringLiteral("rgba(245,158,11,0.12)")
                            : QStringLiteral("rgba(245,158,11,0.10)");
            border = dark_mode_ ? QStringLiteral("rgba(245,158,11,0.24)")
                                : QStringLiteral("rgba(245,158,11,0.18)");
            text_color = QStringLiteral("#F59E0B");
            break;
        case ValueTone::Muted:
            break;
        case ValueTone::Error:
            bg = dark_mode_ ? QStringLiteral("rgba(251,44,54,0.10)")
                            : QStringLiteral("rgba(239,68,68,0.10)");
            border = dark_mode_ ? QStringLiteral("rgba(251,44,54,0.20)")
                                : QStringLiteral("rgba(239,68,68,0.20)");
            text_color = dark_mode_ ? QStringLiteral("#FF6467") : QStringLiteral("#DC2626");
            break;
    }

    top_motors_chip_->setStyleSheet(
        QStringLiteral("background: %1; border: 1px solid %2; border-radius: 4px;")
            .arg(bg, border));
    lbl_top_motors_dot_->setPixmap(loadSvgPixmap(
        QStringLiteral(":/assets/missionplanner/motors_armed_dot.svg"), 6, 6, text_color));
    lbl_top_motors_text_->setText(text);
    lbl_top_motors_text_->setStyleSheet(
        QStringLiteral("font-family: 'Arimo'; font-size: 10px; font-weight: 700; color: %1; "
                       "letter-spacing: 0.5px; background: transparent;")
            .arg(text_color));
    updateTopMotorsChipGeometry();
}

void ExplorationScreen::setTelemetrySpeedMps(double speed_mps) {
    if (!lbl_telemetry_speed_) {
        return;
    }
    lbl_telemetry_speed_->setText(QString("%1 m/s").arg(std::max(0.0, speed_mps), 0, 'f', 2));
}

void ExplorationScreen::setTelemetryPositionMeters(double x_m, double y_m) {
    if (!lbl_telemetry_position_) {
        return;
    }
    lbl_telemetry_position_->setText(
        QString("x: %1 m\ny: %2 m").arg(x_m, 0, 'f', 2).arg(y_m, 0, 'f', 2));
}

void ExplorationScreen::setTelemetryAltitudeMeters(double z_m) {
    if (!lbl_telemetry_altitude_) {
        return;
    }
    lbl_telemetry_altitude_->setText(QString("%1 m").arg(z_m, 0, 'f', 2));
}

void ExplorationScreen::setTelemetryScanTimeSeconds(int elapsed_seconds) {
    if (!lbl_telemetry_scan_time_) {
        return;
    }
    const int total = std::max(0, elapsed_seconds);
    const int hours = total / 3600;
    const int minutes = (total % 3600) / 60;
    const int seconds = total % 60;
    QString text;
    if (hours > 0) {
        text = QString("%1:%2:%3")
                   .arg(hours, 2, 10, QLatin1Char('0'))
                   .arg(minutes, 2, 10, QLatin1Char('0'))
                   .arg(seconds, 2, 10, QLatin1Char('0'));
    } else {
        text = QString("%1:%2")
                   .arg(minutes, 2, 10, QLatin1Char('0'))
                   .arg(seconds, 2, 10, QLatin1Char('0'));
    }
    lbl_telemetry_scan_time_->setText(text);
}

void ExplorationScreen::setFpvSpeedMps(double speed_mps) {
    if (!lbl_fpv_speed_overlay_) {
        return;
    }
    lbl_fpv_speed_overlay_->setText(
        QString("<span style=\"font-size:30px; line-height:36px; color:#FFFFFF;\">%1 </span>"
                "<span style=\"font-size:18px; line-height:28px; color:#9F9FA9;\">m/s</span>")
            .arg(std::max(0.0, speed_mps), 0, 'f', 1));
}

void ExplorationScreen::setThermalSummary(double max_c,
                                          double avg_c,
                                          double min_c,
                                          const QString& state_text,
                                          ValueTone state_tone) {
    Q_UNUSED(min_c);
    if (thermal_summary_panel_) {
        thermal_summary_panel_->show();
    }
    if (lbl_thermal_max_) {
        lbl_thermal_max_->setText(QString("Max: %1 C").arg(max_c, 0, 'f', 1));
    }
    if (lbl_thermal_avg_) {
        lbl_thermal_avg_->setText(QString("Avg: %1 C").arg(avg_c, 0, 'f', 1));
    }
    if (lbl_thermal_stale_) {
        lbl_thermal_stale_->hide();
    }
    if (!lbl_thermal_state_) {
        return;
    }

    QString fg = "#9F9FA9";
    QString bg = "rgba(82, 82, 91, 0.55)";
    switch (state_tone) {
        case ValueTone::Good:
            fg = "#10B981";
            bg = "rgba(16, 185, 129, 0.15)";
            break;
        case ValueTone::Warning:
            fg = "#F59E0B";
            bg = "rgba(245, 158, 11, 0.18)";
            break;
        case ValueTone::Muted:
            fg = "#9F9FA9";
            bg = "rgba(82, 82, 91, 0.55)";
            break;
        case ValueTone::Error:
            fg = "#EF4444";
            bg = "rgba(239, 68, 68, 0.16)";
            break;
    }

    lbl_thermal_state_->setText(state_text);
    lbl_thermal_state_->setStyleSheet(
        QString("color: %1; background: %2; border-radius: 4px; "
                "font-family: \"Liberation Mono\"; font-size: 12px; line-height: 16px; "
                "font-weight: 700; padding: 3px 8px;")
            .arg(fg, bg));
}

void ExplorationScreen::setThermalUnavailable() {
    if (thermal_summary_panel_) {
        thermal_summary_panel_->show();
    }
    if (lbl_thermal_max_) {
        lbl_thermal_max_->setText("Max: --.- C");
    }
    if (lbl_thermal_avg_) {
        lbl_thermal_avg_->setText("Avg: --.- C");
    }
    if (lbl_thermal_stale_) {
        lbl_thermal_stale_->hide();
    }
    if (!lbl_thermal_state_) {
        return;
    }
    lbl_thermal_state_->setText("Unavailable");
    lbl_thermal_state_->setStyleSheet(
        "color: #9F9FA9; background: rgba(82, 82, 91, 0.55); border-radius: 4px; "
        "font-family: \"Liberation Mono\"; font-size: 12px; line-height: 16px; "
        "font-weight: 700; padding: 3px 8px;");
}

void ExplorationScreen::setThermalHidden() {
    if (lbl_thermal_max_) {
        lbl_thermal_max_->setText("Max: --.- C");
    }
    if (lbl_thermal_avg_) {
        lbl_thermal_avg_->setText("Avg: --.- C");
    }
    if (lbl_thermal_stale_) {
        lbl_thermal_stale_->hide();
    }
    if (thermal_pixel_widget_) {
        thermal_pixel_widget_->resetToEmpty();
    }
    if (lbl_thermal_state_) {
        lbl_thermal_state_->setText("Unavailable");
        lbl_thermal_state_->setStyleSheet(
            "color: #9F9FA9; background: rgba(82, 82, 91, 0.55); border-radius: 4px; "
            "font-family: \"Liberation Mono\"; font-size: 12px; line-height: 16px; "
            "font-weight: 700; padding: 3px 8px;");
    }
    if (thermal_summary_panel_) {
        thermal_summary_panel_->hide();
    }
}

void ExplorationScreen::setThermalThumbnailData(const QByteArray& packed_thumb) {
    if (!thermal_pixel_widget_) {
        return;
    }
    thermal_pixel_widget_->setPackedThumbnail(packed_thumb);
}

void ExplorationScreen::setThermalThumbnailStale(bool stale) {
    if (!lbl_thermal_stale_) {
        return;
    }
    lbl_thermal_stale_->setVisible(stale);
}

void ExplorationScreen::resetNavigationMap() {
    if (!nav_map_widget_) {
        return;
    }
    nav_map_widget_->resetToEmpty();
}

void ExplorationScreen::setNavigationMapData(const QByteArray& packed_grid) {
    if (!nav_map_widget_) {
        return;
    }
    nav_map_widget_->setPackedGrid(packed_grid);
}

void ExplorationScreen::setNavigationMapYawRadians(double yaw_rad) {
    if (!nav_map_widget_) {
        return;
    }
    nav_map_widget_->setYawRadians(yaw_rad);
}

void ExplorationScreen::setNavigationMapStale(bool stale) {
    if (!nav_map_widget_) {
        return;
    }
    nav_map_widget_->setStale(stale);
}

void ExplorationScreen::setHardwareStatus(const QString& motors_text,
                                          ValueTone motors_tone,
                                          const QString& lidar_text,
                                          ValueTone lidar_tone,
                                          const QString& rf_text,
                                          ValueTone rf_tone,
                                          const QString& storage_text,
                                          ValueTone storage_tone) {
    if (lbl_hw_motors_) {
        lbl_hw_motors_->setText(motors_text);
        applyToneToLabel(lbl_hw_motors_, motors_tone, motors_tone != ValueTone::Muted);
    }
    if (lbl_hw_lidar_) {
        lbl_hw_lidar_->setText(lidar_text);
        applyToneToLabel(lbl_hw_lidar_, lidar_tone, lidar_tone != ValueTone::Muted);
    }
    if (lbl_hw_rf_) {
        lbl_hw_rf_->setText(rf_text);
        applyToneToLabel(lbl_hw_rf_, rf_tone, rf_tone != ValueTone::Muted);
    }
    if (lbl_hw_storage_) {
        lbl_hw_storage_->setText(storage_text);
        applyToneToLabel(lbl_hw_storage_, storage_tone, storage_tone != ValueTone::Muted);
    }
}

void ExplorationScreen::onDashboardClicked() {
    emit backRequested();
}

void ExplorationScreen::onStartScanClicked() {
    if (primary_action_state_ == PrimaryActionState::StartMapping) {
        emit startScanRequested();
    } else if (primary_action_state_ == PrimaryActionState::FinishAndSaveMap) {
        emit finishSaveMapRequested();
    }
}

void ExplorationScreen::onStartPlanningClicked() {
    // TEMP(planner-preview): allow opening Mission Planner before a saved map is ready
    // so the planner screen can be visually reviewed. Restore the planning_enabled_
    // gate after planner UI validation is complete.
    emit startPlanningRequested();
}

void ExplorationScreen::onStopPipelineClicked() {
    emit stopPipelineRequested();
}

void ExplorationScreen::onMappingLockTick() {
    if (primary_action_state_ != PrimaryActionState::RunningLocked || mapping_lock_started_at_ms_ <= 0) {
        if (mapping_lock_timer_) {
            mapping_lock_timer_->stop();
        }
        return;
    }

    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    const int elapsed_sec = static_cast<int>(std::max<qint64>(0, (now_ms - mapping_lock_started_at_ms_) / 1000));
    const int remaining_sec = std::max(0, mapping_lock_duration_sec_ - elapsed_sec);

    if (btn_start_scan_text_) {
        btn_start_scan_text_->setText(
            QString("Running... (%1s / %2s)").arg(elapsed_sec).arg(mapping_lock_duration_sec_));
    }
    if (lbl_launch_status_ && launch_ready_) {
        lbl_launch_status_->setText(
            QString("Mapping in progress (%1s remaining before save unlock)").arg(remaining_sec));
    }

    if (elapsed_sec >= mapping_lock_duration_sec_) {
        setPrimaryActionReadyToFinish();
        if (mapping_lock_timer_) {
            mapping_lock_timer_->stop();
        }
    }
}

void ExplorationScreen::onTeleopPublishTick() {
    if (!fpv_control_active_ || !teleopMovementAllowed()) {
        return;
    }
    emitTeleopTwistCommand();
}

void ExplorationScreen::setFpvControlActive(bool active) {
    const bool had_keys_down = key_w_down_ || key_a_down_ || key_s_down_ || key_d_down_;
    if (fpv_control_active_ == active && (!had_keys_down || active)) {
        updateFpvControlIndicator();
        return;
    }

    fpv_control_active_ = active;
    if (!active) {
        key_w_down_ = false;
        key_a_down_ = false;
        key_s_down_ = false;
        key_d_down_ = false;
        emitZeroTeleopTwist();
    } else {
        setFocus(Qt::OtherFocusReason);
    }

    updateFpvControlIndicator();
}

void ExplorationScreen::updateFpvControlIndicator() {
    if (!lbl_fpv_control_state_) {
        return;
    }

    if (fpv_control_active_ && teleopMovementAllowed()) {
        lbl_fpv_control_state_->setText(
            QString("FPV Control: Active (%1 rad/s)").arg(teleop_angular_speed_rps_, 0, 'f', 1));
        lbl_fpv_control_state_->setStyleSheet(
            "color: #10B981; font-size: 12px; line-height: 16px; font-weight: 700;");
    } else if (fpv_control_active_ && !teleopMovementAllowed()) {
        lbl_fpv_control_state_->setText("FPV Control: Waiting for launch...");
        lbl_fpv_control_state_->setStyleSheet(
            "color: #D97706; font-size: 12px; line-height: 16px; font-weight: 700;");
    } else {
        lbl_fpv_control_state_->setText("FPV Control: Inactive");
        lbl_fpv_control_state_->setStyleSheet(
            "color: #9F9FA9; font-size: 12px; line-height: 16px; font-weight: 600;");
    }
}

void ExplorationScreen::emitTeleopTwistCommand() {
    if (!teleopMovementAllowed()) {
        return;
    }

    double linear_x = 0.0;
    double angular_z = 0.0;
    if (key_w_down_ && !key_s_down_) {
        linear_x = teleop_linear_speed_mps_;
    } else if (key_s_down_ && !key_w_down_) {
        linear_x = -teleop_linear_speed_mps_;
    }
    if (key_a_down_ && !key_d_down_) {
        angular_z = teleop_angular_speed_rps_;
    } else if (key_d_down_ && !key_a_down_) {
        angular_z = -teleop_angular_speed_rps_;
    }

    emit teleopTwistRequested(linear_x, angular_z);
}

void ExplorationScreen::emitZeroTeleopTwist() {
    emit teleopTwistRequested(0.0, 0.0);
}

bool ExplorationScreen::teleopMovementAllowed() const {
    return launch_ready_ && !launch_in_progress_ && !loading_overlay_visible_;
}

bool ExplorationScreen::isDescendantOfFpv(const QWidget* widget) const {
    if (!widget || !fpv_focus_target_) {
        return false;
    }
    const QWidget* cursor = widget;
    while (cursor) {
        if (cursor == fpv_focus_target_) {
            return true;
        }
        cursor = cursor->parentWidget();
    }
    return false;
}

bool ExplorationScreen::eventFilter(QObject* watched, QEvent* event) {
    if (!event) {
        return QWidget::eventFilter(watched, event);
    }

    if (event->type() == QEvent::MouseButtonPress && watched == this) {
        // Determine active focus from the top-level click position only once.
        // This avoids parent/child propagation flipping state back to inactive.
        const auto* mouse_event = static_cast<const QMouseEvent*>(event);
        QWidget* clicked = childAt(mouse_event->pos());
        setFpvControlActive(isDescendantOfFpv(clicked));
    } else if (event->type() == QEvent::KeyPress && watched != this) {
        auto* key_event = static_cast<QKeyEvent*>(event);
        keyPressEvent(key_event);
        if (key_event->isAccepted()) {
            return true;
        }
    } else if (event->type() == QEvent::KeyRelease && watched != this) {
        auto* key_event = static_cast<QKeyEvent*>(event);
        keyReleaseEvent(key_event);
        if (key_event->isAccepted()) {
            return true;
        }
    }

    return QWidget::eventFilter(watched, event);
}

void ExplorationScreen::keyPressEvent(QKeyEvent* event) {
    if (!event) {
        return;
    }
    if (event->isAutoRepeat()) {
        event->ignore();
        return;
    }

    if (event->key() == Qt::Key_Q) {
        emit teleopDisarmRequested();
        event->accept();
        return;
    }

    if (!fpv_control_active_ || !teleopMovementAllowed()) {
        QWidget::keyPressEvent(event);
        return;
    }

    switch (event->key()) {
        case Qt::Key_W:
            key_w_down_ = true;
            emitTeleopTwistCommand();
            event->accept();
            return;
        case Qt::Key_A:
            key_a_down_ = true;
            emitTeleopTwistCommand();
            event->accept();
            return;
        case Qt::Key_S:
            key_s_down_ = true;
            emitTeleopTwistCommand();
            event->accept();
            return;
        case Qt::Key_D:
            key_d_down_ = true;
            emitTeleopTwistCommand();
            event->accept();
            return;
        case Qt::Key_E:
            emit teleopArmRequested();
            event->accept();
            return;
        case Qt::Key_O:
            emit teleopGprPowerOffRequested();
            event->accept();
            return;
        case Qt::Key_0:
            teleop_angular_speed_rps_ =
                std::min(teleop_angular_speed_max_rps_, teleop_angular_speed_rps_ + teleop_angular_speed_step_rps_);
            updateFpvControlIndicator();
            event->accept();
            return;
        case Qt::Key_9:
            teleop_angular_speed_rps_ =
                std::max(teleop_angular_speed_min_rps_, teleop_angular_speed_rps_ - teleop_angular_speed_step_rps_);
            updateFpvControlIndicator();
            event->accept();
            return;
        default:
            break;
    }

    QWidget::keyPressEvent(event);
}

void ExplorationScreen::keyReleaseEvent(QKeyEvent* event) {
    if (!event) {
        return;
    }
    if (event->isAutoRepeat()) {
        event->ignore();
        return;
    }

    bool handled = false;
    switch (event->key()) {
        case Qt::Key_W:
            key_w_down_ = false;
            handled = true;
            break;
        case Qt::Key_A:
            key_a_down_ = false;
            handled = true;
            break;
        case Qt::Key_S:
            key_s_down_ = false;
            handled = true;
            break;
        case Qt::Key_D:
            key_d_down_ = false;
            handled = true;
            break;
        default:
            break;
    }

    if (handled) {
        if (fpv_control_active_ && teleopMovementAllowed()) {
            emitTeleopTwistCommand();
        } else {
            emitZeroTeleopTwist();
        }
        event->accept();
        return;
    }

    QWidget::keyReleaseEvent(event);
}

void ExplorationScreen::focusOutEvent(QFocusEvent* event) {
    setFpvControlActive(false);
    QWidget::focusOutEvent(event);
}

void ExplorationScreen::hideEvent(QHideEvent* event) {
    setFpvControlActive(false);
    QWidget::hideEvent(event);
}

void ExplorationScreen::setLaunchInProgress(bool in_progress) {
    launch_in_progress_ = in_progress;
    if (in_progress) {
        setFpvControlActive(false);
    }
    setLoadingOverlayVisible(in_progress);
    if (btn_start_scan_) {
        btn_start_scan_->setEnabled(primary_action_enabled_by_state_ && !in_progress);
    }
    if (lbl_standby_) {
        if (launch_ready_) {
            lbl_standby_->setText("Ready");
        } else if (in_progress) {
            lbl_standby_->setText("Starting");
        } else {
            lbl_standby_->setText("Standby");
        }
    }
}

void ExplorationScreen::setLaunchProgress(int percent, const QString& status_text) {
    const int clamped = std::max(0, std::min(100, percent));
    const QString normalized_status =
        status_text.trimmed().isEmpty() ? QStringLiteral("Launching pipeline...") : status_text.trimmed();
    const QString percent_text = QString("%1%").arg(clamped);

    const bool progress_changed = launch_progress_percent_ != clamped;
    const bool status_changed = launch_status_text_ != normalized_status;
    const bool percent_label_changed = !lbl_launch_percent_ || lbl_launch_percent_->text() != percent_text;
    const bool status_label_changed = !lbl_launch_status_ || lbl_launch_status_->text() != normalized_status;
    const bool bar_changed = !launch_progress_bar_ || launch_progress_bar_->value() != clamped;
    if (!progress_changed && !status_changed && !percent_label_changed && !status_label_changed &&
        !bar_changed) {
        return;
    }

    launch_progress_percent_ = clamped;
    launch_status_text_ = normalized_status;

    if (launch_progress_bar_ && bar_changed) {
        launch_progress_bar_->setValue(clamped);
    }
    if (lbl_launch_percent_ && percent_label_changed) {
        lbl_launch_percent_->setText(percent_text);
    }
    if (lbl_launch_status_ && status_label_changed) {
        lbl_launch_status_->setText(normalized_status);
    }
    updateLoadingOverlayText();
}

void ExplorationScreen::setLaunchDiagnostics(const QString& diagnostics_text) {
    if (!launch_diagnostics_view_) {
        return;
    }
    if (launch_diagnostics_view_->toPlainText() == diagnostics_text) {
        return;
    }
    launch_diagnostics_view_->setPlainText(diagnostics_text);
    launch_diagnostics_view_->moveCursor(QTextCursor::Start);
}

void ExplorationScreen::setLoadingOverlayVisible(bool visible) {
    if (loading_overlay_visible_ == visible) {
        return;
    }
    loading_overlay_visible_ = visible;
    if (visible) {
        setFpvControlActive(false);
    }

    if (btn_stop_pipeline_) {
        btn_stop_pipeline_->setVisible(!visible);
    }
    if (launch_progress_card_) {
        launch_progress_card_->setVisible(!visible);
    }

    if (content_root_) {
        if (visible) {
            if (!content_blur_effect_) {
                content_blur_effect_ = new QGraphicsBlurEffect(content_root_);
                content_blur_effect_->setBlurRadius(10.0);
            }
            content_root_->setGraphicsEffect(content_blur_effect_);
            content_root_->setEnabled(false);
        } else {
            content_root_->setGraphicsEffect(nullptr);
            content_blur_effect_ = nullptr;
            content_root_->setEnabled(true);
        }
    }

    if (loading_overlay_) {
        if (visible) {
            updateLoadingOverlayGeometry();
            updateLoadingOverlayText();
            loading_overlay_->show();
            loading_overlay_->raise();
            loading_overlay_->setFocus(Qt::OtherFocusReason);
        } else {
            loading_overlay_->hide();
        }
    }

    if (loading_overlay_) {
        if (visible) {
            loading_overlay_->startAnimation();
        } else {
            loading_overlay_->stopAnimation();
        }
    }
}

void ExplorationScreen::updateLoadingOverlayGeometry() {
    if (!loading_overlay_) {
        return;
    }
    loading_overlay_->setGeometry(rect());
}

void ExplorationScreen::updateLoadingOverlayText() {
    if (!loading_overlay_) {
        return;
    }

    loading_overlay_->setStatusText(launch_status_text_);
    loading_overlay_->setPercent(launch_progress_percent_);
}

void ExplorationScreen::setLaunchReady(bool ready) {
    launch_ready_ = ready;
    if (ready) {
        setLaunchInProgress(false);
        setLaunchProgress(100, "All systems ready");
    } else {
        setFpvControlActive(false);
        if (!launch_in_progress_) {
            setLaunchProgress(0, "Waiting for Start Scan");
        }
        if (lbl_standby_ && !launch_in_progress_) {
            lbl_standby_->setText("Standby");
        }
    }
    updateFpvControlIndicator();
}

void ExplorationScreen::setPrimaryActionState(PrimaryActionState state) {
    primary_action_state_ = state;
    refreshPrimaryActionButton();
}

void ExplorationScreen::refreshPrimaryActionButton() {
    if (!btn_start_scan_ || !btn_start_scan_text_) {
        return;
    }

    // Each state below routes its label through setPrimaryActionLabel() so
    // the button width snugs to the new text. RunningLocked is special:
    // pre-sized once for "Running... (60s / 60s)" so the per-second
    // onMappingLockTick() text update doesn't bounce the button width as
    // the digit count crosses 9 → 10 / 9 → 60.
    QString button_style;
    switch (primary_action_state_) {
        case PrimaryActionState::StartMapping:
            primary_action_enabled_by_state_ = true;
            setPrimaryActionLabel(QStringLiteral("Start Mapping"));
            button_style =
                "QPushButton { background: #10B981; border: none; border-radius: 10px; }"
                "QPushButton:hover:enabled { background: #34D399; }"
                "QPushButton:disabled { background: #334155; }";
            break;
        case PrimaryActionState::RunningLocked:
            primary_action_enabled_by_state_ = false;
            // Pre-size for the worst-case digit count (60s / 60s), then
            // overwrite the text with the actual t=0 string. Subsequent
            // per-tick setText calls in onMappingLockTick() leave the
            // button width untouched.
            setPrimaryActionLabel(QStringLiteral("Running... (60s / 60s)"));
            btn_start_scan_text_->setText(QStringLiteral("Running... (0s / 60s)"));
            button_style =
                "QPushButton { background: #27272A; border: none; border-radius: 10px; }"
                "QPushButton:disabled { background: #27272A; }";
            break;
        case PrimaryActionState::FinishAndSaveMap:
            primary_action_enabled_by_state_ = true;
            setPrimaryActionLabel(QStringLiteral("Finish & Save Map"));
            button_style =
                "QPushButton { background: #3B82F6; border: none; border-radius: 10px; }"
                "QPushButton:hover:enabled { background: #2563EB; }"
                "QPushButton:disabled { background: #334155; }";
            break;
        case PrimaryActionState::SavingMap:
            primary_action_enabled_by_state_ = false;
            setPrimaryActionLabel(QStringLiteral("Saving map..."));
            button_style =
                "QPushButton { background: #1F2937; border: none; border-radius: 10px; }"
                "QPushButton:disabled { background: #1F2937; }";
            break;
        case PrimaryActionState::DownloadingMap:
            primary_action_enabled_by_state_ = false;
            setPrimaryActionLabel(QStringLiteral("Downloading map..."));
            button_style =
                "QPushButton { background: #1F2937; border: none; border-radius: 10px; }"
                "QPushButton:disabled { background: #1F2937; }";
            break;
        case PrimaryActionState::MapReady:
            primary_action_enabled_by_state_ = false;
            setPrimaryActionLabel(QStringLiteral("Map Ready"));
            button_style =
                "QPushButton { background: #0F766E; border: none; border-radius: 10px; }"
                "QPushButton:disabled { background: #0F766E; }";
            break;
    }

    btn_start_scan_->setStyleSheet(button_style);
    btn_start_scan_->setEnabled(primary_action_enabled_by_state_ && !launch_in_progress_);
}

void ExplorationScreen::setPrimaryActionLabel(const QString& text) {
    if (!btn_start_scan_ || !btn_start_scan_text_) {
        return;
    }
    btn_start_scan_text_->setText(text);
    const int text_w =
        QFontMetrics(btn_start_scan_text_->font()).horizontalAdvance(text);
    btn_start_scan_->setFixedWidth(
        kExplActionButtonChrome + text_w + kExplActionButtonSafetyPad);
}

void ExplorationScreen::resetMappingWorkflowUi() {
    if (mapping_lock_timer_) {
        mapping_lock_timer_->stop();
    }
    mapping_lock_started_at_ms_ = 0;
    mapping_lock_duration_sec_ = 60;
    setPrimaryActionState(PrimaryActionState::StartMapping);
}

void ExplorationScreen::beginMappingRunLock(int min_seconds) {
    mapping_lock_duration_sec_ = std::max(1, min_seconds);
    mapping_lock_started_at_ms_ = QDateTime::currentMSecsSinceEpoch();
    setPrimaryActionState(PrimaryActionState::RunningLocked);
    if (mapping_lock_timer_) {
        mapping_lock_timer_->start();
    }
    onMappingLockTick();
}

void ExplorationScreen::setPrimaryActionReadyToFinish() {
    if (mapping_lock_timer_) {
        mapping_lock_timer_->stop();
    }
    mapping_lock_started_at_ms_ = 0;
    setPrimaryActionState(PrimaryActionState::FinishAndSaveMap);
}

void ExplorationScreen::showMapSaveInProgress() {
    setPrimaryActionState(PrimaryActionState::SavingMap);
}

void ExplorationScreen::showMapDownloadInProgress() {
    setPrimaryActionState(PrimaryActionState::DownloadingMap);
}

void ExplorationScreen::showMapReady() {
    if (mapping_lock_timer_) {
        mapping_lock_timer_->stop();
    }
    setPrimaryActionState(PrimaryActionState::MapReady);
}

void ExplorationScreen::setPlanningEnabled(bool enabled) {
    planning_enabled_ = enabled;
    if (btn_start_planning_) {
        // TEMP(planner-preview): keep the button enabled even when the saved-map
        // precondition is not satisfied. Restore the real enabled state later.
        btn_start_planning_->setEnabled(true);
        btn_start_planning_->setToolTip(
            planning_enabled_ ? "Open planner stage"
                              : "Open planner stage (temporary map bypass)");
    }
    if (btn_start_planning_text_) {
        btn_start_planning_text_->setText(QStringLiteral("Start Planning"));
    }
}

void ExplorationScreen::startFpvStream(int port) {
    if (!fpv_media_stack_ || !fpv_stream_widget_) {
        return;
    }
    setFpvControlActive(false);
    if (fpv_placeholder_) {
        fpv_media_stack_->setCurrentWidget(fpv_placeholder_);
    }
    fpv_stream_widget_->startStream(port);
}

void ExplorationScreen::stopFpvStream() {
    setFpvControlActive(false);
    if (fpv_stream_widget_) {
        fpv_stream_widget_->stopStream();
    }
    if (fpv_media_stack_ && fpv_placeholder_) {
        fpv_media_stack_->setCurrentWidget(fpv_placeholder_);
    }
}

void ExplorationScreen::forceTeleopStop() {
    setFpvControlActive(false);
}

void ExplorationScreen::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    content_root_ = new QWidget(this);
    content_root_->setObjectName("ExplContentRoot");
    auto* content_layout = new QVBoxLayout(content_root_);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(0);
    root->addWidget(content_root_, 1);

    // Top status strip (matches Figma frame top row).
    auto* top_bar = new QWidget(content_root_);
    top_bar->setObjectName("ExplTopBar");
    top_bar->setFixedHeight(49);
    auto* top_bar_layout = new QHBoxLayout(top_bar);
    top_bar_layout->setContentsMargins(24, 0, 24, 0);
    top_bar_layout->setSpacing(8);

    btn_dashboard_ = new QPushButton(top_bar);
    btn_dashboard_->setObjectName("ExplBackButton");
    btn_dashboard_->setCursor(Qt::PointingHandCursor);
    btn_dashboard_->setFixedSize(40, 28);
    auto* back_layout = new QHBoxLayout(btn_dashboard_);
    back_layout->setContentsMargins(12, 6, 12, 6);
    back_layout->setSpacing(0);
    auto* back_icon = new QWidget(btn_dashboard_);
    back_icon->setFixedSize(16, 16);
    auto* back_head = makeSvgIcon(":/assets/exploration/back_vector_a.svg", 6, 11, back_icon);
    back_head->move(3, 2);
    auto* back_line = makeSvgIcon(":/assets/exploration/back_vector_b.svg", 11, 2, back_icon);
    back_line->move(3, 7);
    back_layout->addWidget(back_icon, 0, Qt::AlignCenter);
    connect(btn_dashboard_, &QPushButton::clicked, this, &ExplorationScreen::onDashboardClicked);
    top_bar_layout->addWidget(btn_dashboard_, 0, Qt::AlignVCenter);
    top_bar_layout->addStretch(1);

    const QString kInitialStatus14 =
        QStringLiteral("font-family: 'Arimo'; font-size: 14px; font-weight: 400; color: #9F9FA9;");

    auto* top_right_host = new QWidget(top_bar);
    top_right_host->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    auto* top_right_layout = new QHBoxLayout(top_right_host);
    top_right_layout->setContentsMargins(0, 0, 0, 0);
    top_right_layout->setSpacing(24);

    auto* status_bar = new QWidget(top_right_host);
    status_bar->setFixedHeight(kTopStatusItemHeight);
    status_bar->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    auto* status_layout = new QHBoxLayout(status_bar);
    status_layout->setContentsMargins(0, 0, 0, 0);
    status_layout->setSpacing(24);
    status_layout->addWidget(makePlannerStatusItem(status_bar,
                                                   QStringLiteral(":/assets/missionplanner/battery.svg"),
                                                   16,
                                                   QStringLiteral("87%"),
                                                   kTopStatusBatteryMinWidth,
                                                   kInitialStatus14,
                                                   QString(),
                                                   &lbl_top_battery_));
    status_layout->addWidget(makePlannerStatusItem(status_bar,
                                                   QStringLiteral(":/assets/missionplanner/status_dot.svg"),
                                                   8,
                                                   top_signal_text_,
                                                   kTopStatusSignalMinWidth,
                                                   kInitialStatus14,
                                                   QString(),
                                                   &lbl_top_signal_));
    auto* lock_item = new QWidget(status_bar);
    lock_item->setObjectName("ExplTopStatusItem");
    lock_item->setFixedHeight(kTopStatusItemHeight);
    lock_item->setMinimumWidth(kTopStatusLockMinWidth);
    lock_item->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    auto* lock_layout = new QHBoxLayout(lock_item);
    lock_layout->setContentsMargins(0, 0, 0, 0);
    lock_layout->setSpacing(8);
    lock_layout->addWidget(
        makePlannerStatusIconLabel(lock_item, QStringLiteral(":/assets/missionplanner/lock.svg"), 16),
        0,
        Qt::AlignVCenter);
    lbl_top_lock_chip_ = makePlannerStatusTextLabel(lock_item, top_lock_text_, kInitialStatus14);
    lock_layout->addWidget(lbl_top_lock_chip_, 0, Qt::AlignVCenter);
    status_layout->addWidget(lock_item);

    top_motors_chip_ = new QWidget(status_bar);
    top_motors_chip_->setFixedSize(kTopStatusMotorsChipMinWidth, kTopStatusMotorsChipHeight);
    top_motors_chip_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    top_motors_chip_->setAttribute(Qt::WA_StyledBackground, true);
    auto* motors_layout = new QHBoxLayout(top_motors_chip_);
    motors_layout->setContentsMargins(kTopStatusMotorsChipHorizontalPadding,
                                      1,
                                      kTopStatusMotorsChipHorizontalPadding,
                                      1);
    motors_layout->setSpacing(kTopStatusMotorsChipSpacing);
    lbl_top_motors_dot_ = makePlannerStatusIconLabel(top_motors_chip_,
                                                     QStringLiteral(":/assets/missionplanner/motors_armed_dot.svg"),
                                                     6,
                                                     QStringLiteral("#71717B"));
    motors_layout->addWidget(lbl_top_motors_dot_, 0, Qt::AlignVCenter);
    lbl_top_motors_text_ = makePlannerStatusTextLabel(
        top_motors_chip_,
        top_motors_text_,
        QStringLiteral("font-family: 'Arimo'; font-size: 10px; font-weight: 700; "
                       "color: #71717B; letter-spacing: 0.5px;"));
    motors_layout->addWidget(lbl_top_motors_text_, 0, Qt::AlignVCenter);
    status_layout->addWidget(top_motors_chip_);

    top_right_layout->addWidget(status_bar, 0, Qt::AlignVCenter);
    auto* window_controls_reserve = new QWidget(top_right_host);
    window_controls_reserve->setFixedWidth(kTopStatusWindowControlsReservedWidth);
    window_controls_reserve->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    top_right_layout->addWidget(window_controls_reserve, 0, Qt::AlignVCenter);
    top_bar_layout->addWidget(top_right_host, 0, Qt::AlignVCenter);

    content_layout->addWidget(top_bar);

    // Title strip (matches Figma frame second row).
    auto* title_bar = new QWidget(content_root_);
    title_bar->setObjectName("ExplTitleBar");
    title_bar->setFixedHeight(61);
    auto* title_bar_layout = new QHBoxLayout(title_bar);
    title_bar_layout->setContentsMargins(24, 0, 24, 0);
    title_bar_layout->setSpacing(8);

    auto* title = new QLabel("Exploration Mode", title_bar);
    title->setObjectName("ExplTitle");
    title_bar_layout->addWidget(title, 0, Qt::AlignVCenter);
    title_bar_layout->addStretch(1);

    auto* standby_wrap = new QWidget(title_bar);
    auto* standby_layout = new QHBoxLayout(standby_wrap);
    standby_layout->setContentsMargins(12, 0, 12, 0);
    standby_layout->setSpacing(8);
    standby_layout->addWidget(makeSvgIcon(":/assets/exploration/standby.svg", 20, 20, standby_wrap));
    lbl_standby_ = new QLabel("Standby", standby_wrap);
    lbl_standby_->setObjectName("ExplStandbyText");
    standby_layout->addWidget(lbl_standby_, 0, Qt::AlignVCenter);
    title_bar_layout->addWidget(standby_wrap, 0, Qt::AlignVCenter);

    content_layout->addWidget(title_bar);

    auto* body = new QWidget(content_root_);
    body->setObjectName("ExplBody");
    auto* body_layout = new QHBoxLayout(body);
    body_layout->setContentsMargins(0, 0, 0, 0);
    body_layout->setSpacing(0);

    constexpr int kLeftSidebarWidth = 320;
    constexpr int kCenterPanelMinWidth = 540;
    constexpr int kFpvMinHeight = 320;
    constexpr int kRightSidebarWidth = 380;
    constexpr int kRightPanelMargin = 12;
    constexpr int kRightPanelSpacing = 10;
    constexpr int kThermalCardHeight = 320;
    constexpr int kMapCardHeight = 396;
    constexpr int kHardwareCardHeight = 144;
    constexpr int kThermalSummaryPanelMinHeight = 48;

    // Left sidebar
    auto* left_panel = new QWidget(body);
    left_panel->setObjectName("ExplLeftPanel");
    left_panel->setFixedWidth(kLeftSidebarWidth);
    auto* left_panel_layout = new QVBoxLayout(left_panel);
    left_panel_layout->setContentsMargins(16, 16, 17, 16);
    left_panel_layout->setSpacing(16);

    auto* telemetry_card = new QWidget(left_panel);
    telemetry_card->setObjectName("ExplDataCard");
    telemetry_card->setFixedSize(287, 291);
    auto* telemetry_layout = new QVBoxLayout(telemetry_card);
    telemetry_layout->setContentsMargins(16, 16, 16, 16);
    telemetry_layout->setSpacing(12);
    telemetry_layout->addWidget(makeHeadingRow(":/assets/exploration/telemetry.svg", "Telemetry", telemetry_card));
    telemetry_layout->addWidget(
        makeMetricRow("Speed", "0.00 m/s", "ExplMetricValueMono", telemetry_card, &lbl_telemetry_speed_));
    telemetry_layout->addWidget(
        makeMetricRow("Position", "x: 0.00 m\ny: 0.00 m", "ExplMetricValueTinyMono", telemetry_card, &lbl_telemetry_position_));
    telemetry_layout->addWidget(
        makeMetricRow("Altitude", "0.00 m", "ExplMetricValueMono", telemetry_card, &lbl_telemetry_altitude_));
    telemetry_layout->addWidget(
        makeMetricRow("Scan Time", "00:00", "ExplMetricValueMono", telemetry_card, &lbl_telemetry_scan_time_));
    telemetry_layout->addStretch(1);
    left_panel_layout->addWidget(telemetry_card);

    launch_progress_card_ = new QWidget(left_panel);
    launch_progress_card_->setObjectName("ExplDataCard");
    launch_progress_card_->setAttribute(Qt::WA_StyledBackground, true);
    launch_progress_card_->setFixedSize(287, 335);
    auto* progress_layout = new QVBoxLayout(launch_progress_card_);
    progress_layout->setContentsMargins(16, 16, 16, 16);
    progress_layout->setSpacing(10);
    progress_layout->addWidget(makeHeadingRow(":/assets/exploration/scan_progress.svg",
                                              "Scan Progress",
                                              launch_progress_card_));

    auto* progress_top_row = new QWidget(launch_progress_card_);
    auto* progress_top_layout = new QHBoxLayout(progress_top_row);
    progress_top_layout->setContentsMargins(0, 0, 0, 0);
    progress_top_layout->setSpacing(8);
    auto* launch_label = new QLabel("Launch Progress", progress_top_row);
    launch_label->setObjectName("ExplMetricLabel");
    progress_top_layout->addWidget(launch_label);
    progress_top_layout->addStretch(1);
    lbl_launch_percent_ = new QLabel("0%", progress_top_row);
    lbl_launch_percent_->setObjectName("ExplMetricValueMono");
    progress_top_layout->addWidget(lbl_launch_percent_);
    progress_layout->addWidget(progress_top_row);

    launch_progress_bar_ = new QProgressBar(launch_progress_card_);
    launch_progress_bar_->setObjectName("ExplLaunchProgressBar");
    launch_progress_bar_->setRange(0, 100);
    launch_progress_bar_->setValue(0);
    launch_progress_bar_->setTextVisible(false);
    launch_progress_bar_->setFixedHeight(8);
    progress_layout->addWidget(launch_progress_bar_);

    lbl_launch_status_ = new QLabel("Waiting for Start Scan", launch_progress_card_);
    lbl_launch_status_->setObjectName("ExplLaunchStatusText");
    lbl_launch_status_->setWordWrap(true);
    progress_layout->addWidget(lbl_launch_status_);

    auto* diagnostics_label = new QLabel("Launch Diagnostics", launch_progress_card_);
    diagnostics_label->setObjectName("ExplMetricLabel");
    progress_layout->addWidget(diagnostics_label);

    launch_diagnostics_view_ = new QPlainTextEdit(launch_progress_card_);
    launch_diagnostics_view_->setObjectName("ExplLaunchDiagnostics");
    launch_diagnostics_view_->setReadOnly(true);
    launch_diagnostics_view_->setFrameStyle(QFrame::NoFrame);
    launch_diagnostics_view_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    launch_diagnostics_view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    launch_diagnostics_view_->setLineWrapMode(QPlainTextEdit::NoWrap);
    launch_diagnostics_view_->setFocusPolicy(Qt::NoFocus);
    launch_diagnostics_view_->setMinimumHeight(130);
    AutoHideScrollBar::install(launch_diagnostics_view_, dark_mode_);
    progress_layout->addWidget(launch_diagnostics_view_, 1);
    left_panel_layout->addWidget(launch_progress_card_);

    btn_stop_pipeline_ = new QPushButton("Stop Pipeline (Test)", left_panel);
    btn_stop_pipeline_->setObjectName("ExplStopPipelineButton");
    btn_stop_pipeline_->setCursor(Qt::PointingHandCursor);
    btn_stop_pipeline_->setFixedHeight(40);
    btn_stop_pipeline_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    btn_stop_pipeline_->setToolTip("Temporary test control: stop laptop and robot pipeline processes");
    left_panel_layout->addWidget(btn_stop_pipeline_);
    connect(btn_stop_pipeline_, &QPushButton::clicked, this, &ExplorationScreen::onStopPipelineClicked);
    left_panel_layout->addStretch(1);

    body_layout->addWidget(left_panel);

    // Center panel
    auto* center_panel = new QWidget(body);
    center_panel->setObjectName("ExplCenterPanel");
    center_panel->setMinimumWidth(kCenterPanelMinWidth);
    auto* center_layout = new QVBoxLayout(center_panel);
    center_layout->setContentsMargins(0, 0, 0, 0);
    center_layout->setSpacing(0);

    auto* fpv = new QWidget(center_panel);
    fpv->setObjectName("ExplFpvArea");
    fpv->setMinimumHeight(kFpvMinHeight);
    fpv_focus_target_ = fpv;
    auto* fpv_stack = new QStackedLayout(fpv);
    fpv_stack->setContentsMargins(0, 0, 0, 0);
    fpv_stack->setStackingMode(QStackedLayout::StackAll);

    fpv_media_stack_ = new QStackedWidget(fpv);
    fpv_media_stack_->setObjectName("ExplFpvMediaStack");
    fpv_media_stack_->setContentsMargins(0, 0, 0, 0);
    fpv_media_stack_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    fpv_placeholder_ = makeFeedImage(":/assets/exploration/fpv_feed.svg", fpv_media_stack_, "ExplFpvBackground");
    fpv_media_stack_->addWidget(fpv_placeholder_);
    fpv_stream_widget_ = new VideoStreamWidget(fpv_media_stack_);
    fpv_stream_widget_->setObjectName("ExplFpvStream");
    fpv_stream_widget_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    fpv_stream_widget_->setMinimumSize(320, 240);
    fpv_media_stack_->addWidget(fpv_stream_widget_);
    fpv_media_stack_->setCurrentWidget(fpv_placeholder_);
    connect(fpv_stream_widget_, &VideoStreamWidget::firstFrameReady, this, [this]() {
        if (!fpv_media_stack_ || !fpv_stream_widget_) {
            return;
        }
        fpv_media_stack_->setCurrentWidget(fpv_stream_widget_);
    });
    auto showFpvPlaceholder = [this]() {
        setFpvControlActive(false);
        if (fpv_media_stack_ && fpv_placeholder_) {
            fpv_media_stack_->setCurrentWidget(fpv_placeholder_);
        }
    };
    connect(fpv_stream_widget_, &VideoStreamWidget::streamStopped, this, showFpvPlaceholder);
    connect(fpv_stream_widget_, &VideoStreamWidget::streamError, this, [showFpvPlaceholder](const QString&) {
        showFpvPlaceholder();
    });
    fpv_stack->addWidget(fpv_media_stack_);

    auto* fpv_overlay = new QWidget(fpv);
    auto* fpv_layout = new QGridLayout(fpv_overlay);
    fpv_layout->setContentsMargins(16, 16, 16, 16);
    fpv_layout->setHorizontalSpacing(16);
    fpv_layout->setVerticalSpacing(16);
    fpv_layout->setRowStretch(0, 0);
    fpv_layout->setRowStretch(1, 1);
    fpv_layout->setRowStretch(2, 0);
    fpv_layout->setColumnStretch(0, 1);
    fpv_layout->setColumnStretch(1, 1);

    auto* overlay_left = new QLabel("FPS: 30\nRES: 1920x1080\nFOV: 120°", fpv_overlay);
    overlay_left->setObjectName("ExplFpvStats");
    overlay_left->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    overlay_left->setFixedSize(142, 84);
    fpv_layout->addWidget(overlay_left, 0, 0, Qt::AlignLeft | Qt::AlignTop);

    lbl_fpv_speed_overlay_ =
        new QLabel("<span style=\"font-size:30px; line-height:36px; color:#FFFFFF;\">0.0 </span>"
                   "<span style=\"font-size:18px; line-height:28px; color:#9F9FA9;\">m/s</span>",
                   fpv_overlay);
    lbl_fpv_speed_overlay_->setObjectName("ExplFpvSpeed");
    lbl_fpv_speed_overlay_->setFixedSize(136, 52);
    lbl_fpv_speed_overlay_->setTextFormat(Qt::RichText);
    lbl_fpv_speed_overlay_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    fpv_layout->addWidget(lbl_fpv_speed_overlay_, 0, 1, Qt::AlignRight | Qt::AlignTop);

    auto* crosshair = new QWidget(fpv_overlay);
    crosshair->setObjectName("ExplCrosshair");
    crosshair->setFixedSize(32, 32);
    auto* crosshair_h = new QFrame(crosshair);
    crosshair_h->setObjectName("ExplCrosshairH");
    crosshair_h->setGeometry(0, 15, 32, 1);
    auto* crosshair_v = new QFrame(crosshair);
    crosshair_v->setObjectName("ExplCrosshairV");
    crosshair_v->setGeometry(15, 0, 1, 32);
    fpv_layout->addWidget(crosshair, 1, 0, 1, 2, Qt::AlignCenter);
    fpv_stack->addWidget(fpv_overlay);

    center_layout->addWidget(fpv, 1);

    auto* controls = new QWidget(center_panel);
    controls->setObjectName("ExplControlBar");
    controls->setFixedHeight(81);
    auto* controls_layout = new QHBoxLayout(controls);
    controls_layout->setContentsMargins(24, 16, 24, 16);
    controls_layout->setSpacing(12);

    auto* left_controls = new QWidget(controls);
    auto* left_controls_layout = new QHBoxLayout(left_controls);
    left_controls_layout->setContentsMargins(0, 0, 0, 0);
    left_controls_layout->setSpacing(16);

    btn_start_scan_ = new QPushButton(left_controls);
    btn_start_scan_->setObjectName("ExplStartScanButton");
    btn_start_scan_->setCursor(Qt::PointingHandCursor);
    btn_start_scan_->setFixedHeight(48);
    auto* btn_start_scan_layout = new QHBoxLayout(btn_start_scan_);
    btn_start_scan_layout->setContentsMargins(24, 0, 12, 0);
    btn_start_scan_layout->setSpacing(6);
    btn_start_scan_layout->addWidget(makeSvgIcon(":/assets/exploration/start_scan.svg",
                                                 20,
                                                 20,
                                                 btn_start_scan_,
                                                 "ExplActionButtonIcon"),
                                     0,
                                     Qt::AlignVCenter);
    btn_start_scan_text_ = new QLabel("Start Scan", btn_start_scan_);
    btn_start_scan_text_->setObjectName("ExplActionButtonText");
    QFont action_font("Arimo");
    action_font.setBold(true);
    action_font.setPixelSize(16);
    btn_start_scan_text_->setFont(action_font);
    btn_start_scan_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    btn_start_scan_layout->addWidget(btn_start_scan_text_, 0, Qt::AlignVCenter);
    btn_start_scan_layout->addStretch(1);
    left_controls_layout->addWidget(btn_start_scan_);
    connect(btn_start_scan_, &QPushButton::clicked, this, &ExplorationScreen::onStartScanClicked);
    // Width is sized to the current text by setPrimaryActionLabel(); seed it
    // here so the initial "Start Scan" state renders snug instead of at the
    // QPushButton default sizeHint.
    setPrimaryActionLabel(btn_start_scan_text_->text());

    auto* move_hint = new QLabel("WASD keys to move", left_controls);
    move_hint->setObjectName("ExplMoveHint");
    left_controls_layout->addWidget(move_hint, 0, Qt::AlignVCenter);

    lbl_fpv_control_state_ = new QLabel("FPV Control: Inactive", left_controls);
    lbl_fpv_control_state_->setObjectName("ExplFpvControlState");
    lbl_fpv_control_state_->setToolTip("Click FPV stream to activate keyboard control");
    left_controls_layout->addWidget(lbl_fpv_control_state_, 0, Qt::AlignVCenter);

    controls_layout->addWidget(left_controls);
    controls_layout->addStretch(1);

    btn_start_planning_ = new QPushButton(controls);
    btn_start_planning_->setObjectName("ExplStartPlanningButton");
    btn_start_planning_->setCursor(Qt::PointingHandCursor);
    btn_start_planning_->setFixedHeight(48);
    auto* btn_start_planning_layout = new QHBoxLayout(btn_start_planning_);
    btn_start_planning_layout->setContentsMargins(24, 0, 12, 0);
    btn_start_planning_layout->setSpacing(6);
    btn_start_planning_layout->addWidget(
        makeSvgIcon(":/assets/exploration/start_planning.svg", 20, 20, btn_start_planning_, "ExplActionButtonIcon"),
        0,
        Qt::AlignVCenter);
    btn_start_planning_text_ = new QLabel("Start Planning", btn_start_planning_);
    btn_start_planning_text_->setObjectName("ExplActionButtonText");
    btn_start_planning_text_->setFont(action_font);
    // Single-text CTA — snug-fit via shared chrome + safety pad, with the
    // Figma 185 px floor preserved.
    const QFontMetrics planning_metrics(action_font);
    const int planning_text_w = planning_metrics.horizontalAdvance(btn_start_planning_text_->text());
    const int planning_w = std::max(
        kExplStartPlanningMinWidth,
        kExplActionButtonChrome + planning_text_w + kExplActionButtonSafetyPad);
    btn_start_planning_->setFixedWidth(planning_w);
    btn_start_planning_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    btn_start_planning_layout->addWidget(btn_start_planning_text_, 0, Qt::AlignVCenter);
    btn_start_planning_layout->addStretch(1);
    controls_layout->addWidget(btn_start_planning_, 0, Qt::AlignVCenter);
    connect(btn_start_planning_, &QPushButton::clicked, this, &ExplorationScreen::onStartPlanningClicked);

    center_layout->addWidget(controls);

    body_layout->addWidget(center_panel, 1);

    // Right sidebar
    auto* right_panel = new QWidget(body);
    right_panel->setObjectName("ExplRightPanel");
    right_panel->setFixedWidth(kRightSidebarWidth);
    auto* right_panel_layout = new QVBoxLayout(right_panel);
    right_panel_layout->setContentsMargins(kRightPanelMargin,
                                           kRightPanelMargin,
                                           kRightPanelMargin,
                                           kRightPanelMargin);
    right_panel_layout->setSpacing(kRightPanelSpacing);

    auto* thermal_card = new QWidget(right_panel);
    thermal_card->setObjectName("ExplFeedCard");
    thermal_card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    thermal_card->setFixedHeight(kThermalCardHeight);
    auto* thermal_layout = new QVBoxLayout(thermal_card);
    thermal_layout->setContentsMargins(0, 0, 0, 0);
    thermal_layout->setSpacing(0);
    auto* thermal_header = new QWidget(thermal_card);
    thermal_header->setObjectName("ExplFeedHeader");
    auto* thermal_header_layout = new QHBoxLayout(thermal_header);
    thermal_header_layout->setContentsMargins(12, 0, 12, 0);
    thermal_header_layout->setSpacing(8);
    auto* thermal_title = new QLabel("Thermal Camera", thermal_header);
    thermal_title->setObjectName("ExplFeedTitle");
    thermal_header_layout->addWidget(thermal_title);
    thermal_header_layout->addStretch(1);
    thermal_header_layout->addWidget(makeSvgIcon(":/assets/exploration/thermal.svg", 16, 16, thermal_header));
    thermal_layout->addWidget(thermal_header);

    auto* thermal_view = new QWidget(thermal_card);
    thermal_view->setObjectName("ExplThermalView");
    auto* thermal_stack = new QStackedLayout(thermal_view);
    thermal_stack->setContentsMargins(0, 0, 0, 0);
    thermal_stack->setStackingMode(QStackedLayout::StackAll);
    thermal_pixel_widget_ = new ExplorationThermalPixelWidget(thermal_view);
    thermal_pixel_widget_->setObjectName("ExplThermalPixelWidget");
    thermal_pixel_widget_->setDarkMode(dark_mode_);
    thermal_stack->addWidget(thermal_pixel_widget_);
    auto* thermal_overlay = new QWidget(thermal_view);
    thermal_overlay->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    auto* thermal_view_layout = new QVBoxLayout(thermal_overlay);
    thermal_view_layout->setContentsMargins(10, 10, 10, 10);
    thermal_view_layout->setSpacing(0);
    thermal_summary_panel_ = new QWidget(thermal_overlay);
    thermal_summary_panel_->setObjectName("ExplThermalSummaryPanel");
    thermal_summary_panel_->setAttribute(Qt::WA_StyledBackground, true);
    thermal_summary_panel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    thermal_summary_panel_->setMinimumHeight(kThermalSummaryPanelMinHeight);
    auto* thermal_summary_layout = new QHBoxLayout(thermal_summary_panel_);
    thermal_summary_layout->setContentsMargins(10, 8, 10, 8);
    thermal_summary_layout->setSpacing(8);

    lbl_thermal_state_ = new QLabel("Unavailable", thermal_summary_panel_);
    lbl_thermal_state_->setObjectName("ExplTempChip");
    lbl_thermal_state_->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    thermal_summary_layout->addWidget(lbl_thermal_state_, 0, Qt::AlignVCenter | Qt::AlignLeft);
    thermal_summary_layout->addStretch(1);

    lbl_thermal_stale_ = new QLabel("STALE", thermal_summary_panel_);
    lbl_thermal_stale_->setObjectName("ExplThermalStaleChip");
    lbl_thermal_stale_->setAlignment(Qt::AlignCenter);
    lbl_thermal_stale_->hide();
    thermal_summary_layout->addWidget(lbl_thermal_stale_, 0, Qt::AlignVCenter | Qt::AlignRight);

    auto* thermal_metrics_col = new QWidget(thermal_summary_panel_);
    auto* thermal_metrics_layout = new QVBoxLayout(thermal_metrics_col);
    thermal_metrics_layout->setContentsMargins(0, 0, 0, 0);
    thermal_metrics_layout->setSpacing(0);

    lbl_thermal_max_ = new QLabel("Max: --.- C", thermal_metrics_col);
    lbl_thermal_max_->setObjectName("ExplThermalMetric");
    lbl_thermal_max_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    thermal_metrics_layout->addWidget(lbl_thermal_max_, 0, Qt::AlignRight);

    lbl_thermal_avg_ = new QLabel("Avg: --.- C", thermal_metrics_col);
    lbl_thermal_avg_->setObjectName("ExplThermalMetric");
    lbl_thermal_avg_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    thermal_metrics_layout->addWidget(lbl_thermal_avg_, 0, Qt::AlignRight);

    thermal_summary_layout->addWidget(thermal_metrics_col, 0, Qt::AlignTop | Qt::AlignRight);

    thermal_view_layout->addWidget(thermal_summary_panel_, 0);
    thermal_view_layout->addStretch(1);
    thermal_stack->addWidget(thermal_overlay);
    thermal_stack->setCurrentWidget(thermal_overlay);
    thermal_overlay->raise();
    thermal_layout->addWidget(thermal_view, 1);
    right_panel_layout->addWidget(thermal_card);

    auto* map_card = new QWidget(right_panel);
    map_card->setObjectName("ExplFeedCard");
    map_card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    map_card->setFixedHeight(kMapCardHeight);
    auto* map_layout = new QVBoxLayout(map_card);
    map_layout->setContentsMargins(0, 0, 0, 0);
    map_layout->setSpacing(0);
    auto* map_header = new QWidget(map_card);
    map_header->setObjectName("ExplFeedHeader");
    auto* map_header_layout = new QHBoxLayout(map_header);
    map_header_layout->setContentsMargins(12, 0, 12, 0);
    map_header_layout->setSpacing(8);
    auto* map_title = new QLabel("Navigation Map", map_header);
    map_title->setObjectName("ExplFeedTitle");
    map_header_layout->addWidget(map_title);
    map_header_layout->addStretch(1);
    map_header_layout->addWidget(makeSvgIcon(":/assets/exploration/map.svg", 16, 16, map_header));
    map_layout->addWidget(map_header);
    auto* map_view = new QWidget(map_card);
    map_view->setObjectName("ExplMapView");
    auto* map_view_layout = new QVBoxLayout(map_view);
    map_view_layout->setContentsMargins(0, 0, 0, 0);
    map_view_layout->setSpacing(0);
    nav_map_widget_ = new ExplorationNavMapWidget(map_view);
    nav_map_widget_->setObjectName("ExplNavigationMapWidget");
    nav_map_widget_->setDarkMode(dark_mode_);
    map_view_layout->addWidget(nav_map_widget_);
    map_layout->addWidget(map_view, 1);
    right_panel_layout->addWidget(map_card);

    auto* hardware_card = new QWidget(right_panel);
    hardware_card->setObjectName("ExplDataCard");
    hardware_card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    hardware_card->setFixedHeight(kHardwareCardHeight);
    auto* hardware_layout = new QVBoxLayout(hardware_card);
    hardware_layout->setContentsMargins(16, 16, 16, 16);
    hardware_layout->setSpacing(10);
    hardware_layout->addWidget(makeHeadingRow(":/assets/exploration/hardware.svg", "Hardware Status", hardware_card));

    auto make_hw_row =
        [hardware_card](const QString& key, const QString& val, const QString& val_obj, QLabel** out_value = nullptr) {
        auto* row = new QWidget(hardware_card);
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 0, 0, 0);
        row_layout->setSpacing(8);
        auto* key_lbl = new QLabel(key, row);
        key_lbl->setObjectName("ExplHardwareKey");
        auto* val_lbl = new QLabel(val, row);
        val_lbl->setObjectName(val_obj);
        if (out_value) {
            *out_value = val_lbl;
        }
        row_layout->addWidget(key_lbl);
        row_layout->addStretch(1);
        row_layout->addWidget(val_lbl);
        return row;
    };

    hardware_layout->addWidget(make_hw_row("Motors", "Unknown", "ExplHardwareMuted", &lbl_hw_motors_));
    hardware_layout->addWidget(make_hw_row("LiDAR", "No data", "ExplHardwareMuted", &lbl_hw_lidar_));
    hardware_layout->addWidget(make_hw_row("RF Link", "No stream", "ExplHardwareMuted", &lbl_hw_rf_));
    hardware_layout->addWidget(make_hw_row("Storage", "N/A", "ExplHardwareMuted", &lbl_hw_storage_));
    hardware_layout->addStretch(1);
    right_panel_layout->addWidget(hardware_card);
    right_panel_layout->addStretch(1);

    body_layout->addWidget(right_panel);

    content_layout->addWidget(body, 1);

    loading_overlay_ = new ExplorationLoadingOverlayWidget(this);
    loading_overlay_->setObjectName("ExplLoadingOverlay");
    loading_overlay_->setAttribute(Qt::WA_StyledBackground, true);
    loading_overlay_->setFocusPolicy(Qt::StrongFocus);
    loading_overlay_->setDarkMode(dark_mode_);
    loading_overlay_->setStatusText(launch_status_text_);
    loading_overlay_->setPercent(launch_progress_percent_);
    loading_overlay_->hide();
    updateLoadingOverlayGeometry();
    loading_overlay_->raise();

    // Route top-level input for teleop focus and child key events for WASD handling.
    installEventFilter(this);
    fpv_focus_target_->installEventFilter(this);
    fpv_media_stack_->installEventFilter(this);
    fpv_placeholder_->installEventFilter(this);
    fpv_stream_widget_->installEventFilter(this);
    updateFpvControlIndicator();
}

void ExplorationScreen::applyStyle() {
    const QString bg = dark_mode_ ? "#09090B" : "#F4F4F5";
    const QString header = dark_mode_ ? "#18181B" : "#FFFFFF";
    const QString border = dark_mode_ ? "#27272A" : "#D4D4D8";
    const QString card = dark_mode_ ? "#27272A" : "#F4F4F5";
    const QString title = dark_mode_ ? "#FFFFFF" : "#18181B";
    const QString text = dark_mode_ ? "#FFFFFF" : "#27272A";
    const QString muted = dark_mode_ ? "#9F9FA9" : "#6B7280";
    const QString submuted = dark_mode_ ? "#71717B" : "#6B7280";
    const QString accent = dark_mode_ ? "#00BC7D" : "#059669";
    const QString planning = dark_mode_ ? "#2B7FFF" : "#155DFC";
    const QString feed_header = dark_mode_ ? "#3F3F47" : "#D4D4D8";
    const QString standby = dark_mode_ ? "#52525C" : "#6B7280";

    QString style = QString(R"(
        #ExplorationRoot {
            background: %1;
            font-family: "Arimo";
        }
        #ExplLoadingOverlay {
            background: rgba(0, 0, 0, 0.50);
        }
        #ExplTopBar, #ExplTitleBar {
            background: %2;
            border-bottom: 1px solid %3;
        }
        #ExplBackButton {
            background: transparent;
            border: none;
            border-radius: 10px;
        }
        #ExplBackButton:hover {
            background: rgba(255, 255, 255, 0.06);
        }
        #ExplTitle {
            color: %5;
            font-size: 24px;
            line-height: 36px;
            font-weight: 700;
        }
        #ExplTopStatusText {
            color: %7;
            font-size: 14px;
            line-height: 20px;
        }
        #ExplTopStatusLockText {
            color: %7;
            font-size: 14px;
            line-height: 20px;
            font-weight: 600;
        }
        #ExplStandbyText {
            color: @STANDBY@;
            font-size: 14px;
            line-height: 20px;
        }
        #ExplLeftPanel, #ExplRightPanel {
            background: %2;
        }
        #ExplLeftPanel {
            border-right: 1px solid %3;
        }
        #ExplRightPanel {
            border-left: 1px solid %3;
        }
        #ExplCenterPanel {
            background: #000000;
        }
        #ExplDataCard, #ExplFeedCard {
            background: %4;
            border: none;
            border-radius: 10px;
        }
        #ExplCardHeadingTitle {
            color: %5;
            font-size: 18px;
            line-height: 27px;
            font-weight: 700;
        }
        #ExplMetricLabel {
            color: %8;
            font-size: 14px;
            line-height: 20px;
        }
        #ExplMetricValueMono {
            color: %6;
            font-family: "Liberation Mono";
            font-size: 16px;
            line-height: 24px;
        }
        #ExplMetricValueTinyMono {
            color: %6;
            font-family: "Liberation Mono";
            font-size: 12px;
            line-height: 16px;
        }
        #ExplLaunchProgressBar {
            background: %3;
            border: none;
            border-radius: 4px;
        }
        #ExplLaunchProgressBar::chunk {
            background: %9;
            border-radius: 4px;
        }
        #ExplLaunchStatusText {
            color: %8;
            font-size: 12px;
            line-height: 16px;
        }
        #ExplLaunchDiagnostics {
            background: #0A0A0A;
            border: 1px solid %3;
            border-radius: 8px;
            color: #D4D4D8;
            font-family: "Liberation Mono";
            font-size: 11px;
            line-height: 14px;
            padding: 6px;
        }
        #ExplLaunchDiagnostics QScrollBar:vertical,
        #ExplLaunchDiagnostics QScrollBar:horizontal {
            background: transparent;
            margin: 0px;
        }
        #ExplLaunchDiagnostics QScrollBar::handle:vertical,
        #ExplLaunchDiagnostics QScrollBar::handle:horizontal {
            background: rgba(113, 113, 123, 0.65);
            border-radius: 4px;
        }
        #ExplFpvArea {
            background: #000000;
            border: none;
            border-bottom: 1px solid %3;
        }
        #ExplFpvBackground {
            background: #000000;
        }
        #ExplFpvStats {
            background: rgba(0, 0, 0, 0.5);
            border-radius: 4px;
            color: #00D492;
            font-family: "Liberation Mono";
            font-size: 14px;
            padding: 8px 12px;
        }
        #ExplFpvSpeed {
            background: rgba(0, 0, 0, 0.5);
            border-radius: 4px;
            font-family: "Liberation Mono";
            padding: 0 0 0 16px;
        }
        #ExplCrosshairH, #ExplCrosshairV {
            background: rgba(0, 188, 125, 0.5);
        }
        #ExplControlBar {
            background: %2;
            border-top: 1px solid %3;
        }
        #ExplStartScanButton {
            background: %9;
            border: none;
            border-radius: 10px;
        }
        #ExplStartScanButton:hover {
            background: #00D492;
        }
        #ExplStartPlanningButton {
            background: @PLANNING@;
            border: none;
            border-radius: 10px;
        }
        #ExplStartPlanningButton:hover {
            background: #155DFC;
        }
        #ExplStartPlanningButton:disabled {
            background: #334155;
        }
        #ExplStopPipelineButton {
            background: #DC2626;
            border: none;
            border-radius: 10px;
            color: #FFFFFF;
            font-family: "Arimo";
            font-size: 14px;
            font-weight: 700;
            padding: 0 12px;
        }
        #ExplStopPipelineButton:hover {
            background: #B91C1C;
        }
        #ExplStopPipelineButton:disabled {
            background: #7F1D1D;
        }
        #ExplActionButtonText {
            color: #FFFFFF;
            font-size: 16px;
            line-height: 24px;
            font-weight: 700;
        }
        #ExplMoveHint {
            color: %8;
            font-size: 14px;
            line-height: 20px;
        }
        #ExplFpvControlState {
            color: %8;
            font-size: 12px;
            line-height: 16px;
        }
        #ExplFeedHeader {
            background: @FEED_HEADER@;
            border-top-left-radius: 10px;
            border-top-right-radius: 10px;
            min-height: 36px;
            max-height: 36px;
            color: %6;
            font-size: 14px;
            line-height: 20px;
            font-weight: 700;
        }
        #ExplFeedTitle {
            color: %6;
            font-size: 14px;
            line-height: 20px;
            font-weight: 700;
        }
        #ExplThermalView {
            background: #000000;
            border-bottom-left-radius: 10px;
            border-bottom-right-radius: 10px;
        }
        #ExplThermalSummaryPanel {
            background: rgba(2, 6, 23, 0.64);
            border: 1px solid rgba(148, 163, 184, 0.16);
            border-radius: 10px;
        }
        #ExplTempChip {
            background: rgba(82, 82, 91, 0.55);
            border-radius: 4px;
            color: #D4D4D8;
            font-family: "Liberation Mono";
            font-size: 12px;
            line-height: 16px;
            font-weight: 700;
            padding: 3px 8px;
        }
        #ExplThermalMetric {
            color: #E4E4E7;
            font-family: "Liberation Mono";
            font-size: 12px;
            line-height: 16px;
            font-weight: 700;
        }
        #ExplThermalStaleChip {
            background: rgba(15, 23, 42, 0.88);
            border: 1px solid rgba(148, 163, 184, 0.22);
            border-radius: 4px;
            color: #F8FAFC;
            font-family: "Liberation Mono";
            font-size: 11px;
            line-height: 14px;
            font-weight: 700;
            padding: 2px 6px;
        }
        #ExplMapView {
            background: #000000;
            border-bottom-left-radius: 10px;
            border-bottom-right-radius: 10px;
        }
        #ExplMapBackground {
            background: #000000;
        }
        #ExplMapMarker {
            background: %9;
            border-radius: 8px;
        }
        #ExplHardwareKey {
            color: %8;
            font-size: 14px;
            line-height: 20px;
        }
        #ExplHardwareGood {
            color: %9;
            font-size: 14px;
            line-height: 20px;
        }
        #ExplHardwareMuted {
            color: %7;
            font-size: 14px;
            line-height: 20px;
        }
    )")
                        .arg(bg, header, border, card, title, text, muted, submuted, accent);
    style.replace("@PLANNING@", planning);
    style.replace("@STANDBY@", standby);
    style.replace("@FEED_HEADER@", feed_header);
    setStyleSheet(style);
    if (lbl_top_battery_) {
        const QString battery_text = dark_mode_ ? QStringLiteral("#9F9FA9") : QStringLiteral("#475569");
        lbl_top_battery_->setStyleSheet(
            QStringLiteral("font-family: 'Arimo'; font-size: 14px; font-weight: 400; color: %1; "
                           "background: transparent;")
                .arg(battery_text));
    }
    setTopSignalState(top_signal_text_, top_signal_tone_);
    setTopLockChipState(top_lock_text_, top_lock_tone_);
    setTopMotorsChipState(top_motors_text_, top_motors_tone_);
}

}  // namespace f2c_cpp

