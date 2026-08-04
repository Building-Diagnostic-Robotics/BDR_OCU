#include "satellite_download_dialog.hpp"

#include "satellite_geo_math.hpp"
#include "satellite_map_widget.hpp"
#include "satellite_tile_service.hpp"

#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cmath>

namespace f2c_cpp {

namespace {

// Wide-context floor: a handful of tiles per site, gives the operator a
// zoomed-out orientation view without a meaningful quota cost.
constexpr int kMinDownloadZoom = 13;

constexpr int kParallelFetches = 6;

constexpr double kMetersPerDegreeLat = 111320.0;

}  // namespace

DownloadAreaDialog::DownloadAreaDialog(TileService* tiles, double initial_lat,
                                       double initial_lon, QWidget* parent)
    : QDialog(parent), tiles_(tiles) {
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setModal(true);
    setMinimumWidth(460);
    // Self-contained styling: the main OCU has no application-wide QSS.
    setStyleSheet(QStringLiteral(R"QSS(
QDialog { background: #111827; border: 1px solid #374151; border-radius: 10px; }
QLabel { color: #F3F4F6; font-family: 'Arimo'; }
QLineEdit, QDoubleSpinBox, QSpinBox {
    background: #0f172a; border: 1px solid #374151; border-radius: 6px;
    padding: 5px 8px; color: #F3F4F6; font-family: 'Arimo';
}
QPushButton {
    background: #1f2937; border: 1px solid #374151; border-radius: 6px;
    padding: 6px 14px; color: #F3F4F6; font-family: 'Arimo';
}
QPushButton:hover { background: #273449; }
QPushButton:disabled { color: #6B7280; background: #141a24; }
QPushButton:default { background: #00b35a; border-color: #00b35a; color: #06130b; font-weight: 600; }
QProgressBar {
    background: #0f172a; border: 1px solid #374151; border-radius: 6px;
    text-align: center; color: #F3F4F6;
}
QProgressBar::chunk { background: #00b35a; border-radius: 5px; }
)QSS"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setSpacing(10);

    auto* title = new QLabel(QStringLiteral("Download Satellite Area"), this);
    QFont title_font = title->font();
    title_font.setPointSizeF(title_font.pointSizeF() + 3);
    title_font.setBold(true);
    title->setFont(title_font);
    layout->addWidget(title);

    auto* address_row = new QHBoxLayout();
    address_edit_ = new QLineEdit(this);
    address_edit_->setPlaceholderText(
        QStringLiteral("Job site address (optional)"));
    find_button_ = new QPushButton(QStringLiteral("Find"), this);
    address_row->addWidget(address_edit_, 1);
    address_row->addWidget(find_button_);
    layout->addLayout(address_row);

    auto* form = new QFormLayout();
    form->setSpacing(8);

    lat_spin_ = new QDoubleSpinBox(this);
    lat_spin_->setRange(-85.0, 85.0);
    lat_spin_->setDecimals(6);
    lat_spin_->setValue(initial_lat);
    form->addRow(QStringLiteral("Latitude"), lat_spin_);

    lon_spin_ = new QDoubleSpinBox(this);
    lon_spin_->setRange(-180.0, 180.0);
    lon_spin_->setDecimals(6);
    lon_spin_->setValue(initial_lon);
    form->addRow(QStringLiteral("Longitude"), lon_spin_);

    radius_spin_ = new QSpinBox(this);
    radius_spin_->setRange(100, 3000);
    radius_spin_->setSingleStep(50);
    radius_spin_->setValue(500);
    radius_spin_->setSuffix(QStringLiteral(" m"));
    form->addRow(QStringLiteral("Radius"), radius_spin_);

    max_zoom_spin_ = new QSpinBox(this);
    max_zoom_spin_->setRange(16, SatelliteMapWidget::kMaxZoom);
    max_zoom_spin_->setValue(19);
    form->addRow(QStringLiteral("Max detail (zoom)"), max_zoom_spin_);

    layout->addLayout(form);

    estimate_label_ = new QLabel(this);
    layout->addWidget(estimate_label_);

    progress_ = new QProgressBar(this);
    progress_->setVisible(false);
    layout->addWidget(progress_);

    status_label_ = new QLabel(this);
    status_label_->setWordWrap(true);
    layout->addWidget(status_label_);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    download_button_ = new QPushButton(QStringLiteral("Download"), this);
    download_button_->setDefault(true);
    close_button_ = new QPushButton(QStringLiteral("Close"), this);
    buttons->addWidget(download_button_);
    buttons->addWidget(close_button_);
    layout->addLayout(buttons);

    connect(find_button_, &QPushButton::clicked, this,
            &DownloadAreaDialog::onFindAddress);
    connect(address_edit_, &QLineEdit::returnPressed, this,
            &DownloadAreaDialog::onFindAddress);
    connect(download_button_, &QPushButton::clicked, this,
            &DownloadAreaDialog::onStartDownload);
    connect(close_button_, &QPushButton::clicked, this, &QDialog::reject);

    connect(tiles_, &TileService::tileReady, this,
            [this](int z, int x, int y) { onTileDone(z, x, y, true); });
    connect(tiles_, &TileService::tileFailed, this,
            [this](int z, int x, int y) { onTileDone(z, x, y, false); });

    auto refresh = [this] { refreshEstimate(); };
    connect(lat_spin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, refresh);
    connect(lon_spin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, refresh);
    connect(radius_spin_, QOverload<int>::of(&QSpinBox::valueChanged), this,
            refresh);
    connect(max_zoom_spin_, QOverload<int>::of(&QSpinBox::valueChanged), this,
            refresh);
    refreshEstimate();

    if (!tiles_->hasApiKey()) {
        download_button_->setEnabled(false);
        find_button_->setEnabled(false);
        status_label_->setText(
            QStringLiteral("No ArcGIS API key compiled into this build — "
                           "downloads are disabled."));
    }
}

QVector<DownloadAreaDialog::TileId> DownloadAreaDialog::tilesForArea() const {
    const double lat = lat_spin_->value();
    const double lon = lon_spin_->value();
    const double radius_m = radius_spin_->value();

    const double dlat = radius_m / kMetersPerDegreeLat;
    const double cos_lat =
        std::max(0.01, std::cos(lat * M_PI / 180.0));
    const double dlon = radius_m / (kMetersPerDegreeLat * cos_lat);

    QVector<TileId> out;
    for (int z = kMinDownloadZoom; z <= max_zoom_spin_->value(); ++z) {
        const int n = 1 << z;
        auto tile_x = [n](double lon_deg) {
            return qBound(0, int(std::floor(geo::lonToNormX(lon_deg) * n)),
                          n - 1);
        };
        auto tile_y = [n](double lat_deg) {
            return qBound(0, int(std::floor(geo::latToNormY(lat_deg) * n)),
                          n - 1);
        };
        const int x0 = tile_x(lon - dlon);
        const int x1 = tile_x(lon + dlon);
        const int y0 = tile_y(lat + dlat);  // north edge = smaller y
        const int y1 = tile_y(lat - dlat);
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                out.append({z, x, y});
            }
        }
    }
    return out;
}

void DownloadAreaDialog::refreshEstimate() {
    estimate_label_->setText(
        QStringLiteral("~%1 tiles (zoom %2–%3)")
            .arg(tilesForArea().size())
            .arg(kMinDownloadZoom)
            .arg(max_zoom_spin_->value()));
}

void DownloadAreaDialog::onFindAddress() {
    const QString query = address_edit_->text().trimmed();
    if (query.isEmpty() || downloading_) {
        return;
    }
    status_label_->setText(QStringLiteral("Searching…"));
    find_button_->setEnabled(false);
    tiles_->geocode(query, [this](bool ok, double lat, double lon,
                                  QString label) {
        find_button_->setEnabled(true);
        if (!ok) {
            status_label_->setText(label);
            return;
        }
        lat_spin_->setValue(lat);
        lon_spin_->setValue(lon);
        status_label_->setText(QStringLiteral("Found: %1").arg(label));
    });
}

void DownloadAreaDialog::onStartDownload() {
    if (downloading_) {
        return;
    }
    queue_.clear();
    pending_.clear();
    done_ = 0;
    failed_ = 0;

    int already_cached = 0;
    const QVector<TileId> all = tilesForArea();
    for (const TileId& t : all) {
        if (tiles_->isCached(t.z, t.x, t.y)) {
            ++already_cached;
        } else {
            queue_.append(t);
        }
    }

    if (queue_.isEmpty()) {
        status_label_->setText(
            QStringLiteral("All %1 tiles already cached.").arg(all.size()));
        emit areaReady(lat_spin_->value(), lon_spin_->value());
        return;
    }

    downloading_ = true;
    download_button_->setEnabled(false);
    progress_->setVisible(true);
    progress_->setRange(0, queue_.size());
    progress_->setValue(0);
    status_label_->setText(
        QStringLiteral("Downloading %1 tiles (%2 already cached)…")
            .arg(queue_.size())
            .arg(already_cached));
    startNextFetches();
}

void DownloadAreaDialog::startNextFetches() {
    while (pending_.size() < kParallelFetches && !queue_.isEmpty()) {
        const TileId t = queue_.takeFirst();
        if (tiles_->isCached(t.z, t.x, t.y)) {
            ++done_;
            continue;
        }
        pending_.insert(TileService::tileKey(t.z, t.x, t.y));
        tiles_->fetch(t.z, t.x, t.y);
    }
    if (pending_.isEmpty() && queue_.isEmpty() && downloading_) {
        finishDownload();
    }
}

void DownloadAreaDialog::onTileDone(int z, int x, int y, bool ok) {
    if (!downloading_) {
        return;
    }
    const QString key = TileService::tileKey(z, x, y);
    if (!pending_.remove(key)) {
        return;  // A map-widget fetch, not one of ours.
    }
    ok ? ++done_ : ++failed_;
    progress_->setValue(done_ + failed_);
    startNextFetches();
}

void DownloadAreaDialog::finishDownload() {
    downloading_ = false;
    download_button_->setEnabled(true);
    if (failed_ == 0) {
        status_label_->setText(
            QStringLiteral("Done — %1 tiles cached for offline use.")
                .arg(done_));
    } else {
        status_label_->setText(
            QStringLiteral("Finished with %1 failures (%2 cached). Check "
                           "connectivity and press Download to retry the "
                           "missing tiles.")
                .arg(failed_)
                .arg(done_));
    }
    emit areaReady(lat_spin_->value(), lon_spin_->value());
}

}  // namespace f2c_cpp
