/**
 * @file download_area_dialog.hpp
 * @brief Pre-mission "download area" tool: cache all tiles covering a job
 *        site (address or lat/lon + radius) so the map works offline on the
 *        roof.
 */

#pragma once

#include <QDialog>
#include <QSet>
#include <QVector>

class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;

namespace f2c_cpp {

class TileService;

class DownloadAreaDialog : public QDialog {
    Q_OBJECT

public:
    DownloadAreaDialog(TileService* tiles, double initial_lat,
                       double initial_lon, QWidget* parent = nullptr);

signals:
    /** Emitted when a download finishes so the map can jump to the area. */
    void areaReady(double lat, double lon);

private:
    struct TileId {
        int z;
        int x;
        int y;
    };

    void onFindAddress();
    void onStartDownload();
    void onTileDone(int z, int x, int y, bool ok);
    void startNextFetches();
    void finishDownload();
    void refreshEstimate();
    QVector<TileId> tilesForArea() const;

    TileService* tiles_;

    QLineEdit* address_edit_ = nullptr;
    QPushButton* find_button_ = nullptr;
    QDoubleSpinBox* lat_spin_ = nullptr;
    QDoubleSpinBox* lon_spin_ = nullptr;
    QSpinBox* radius_spin_ = nullptr;
    QSpinBox* max_zoom_spin_ = nullptr;
    QLabel* estimate_label_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* status_label_ = nullptr;
    QPushButton* download_button_ = nullptr;
    QPushButton* close_button_ = nullptr;

    QVector<TileId> queue_;
    QSet<QString> pending_;
    int done_ = 0;
    int failed_ = 0;
    bool downloading_ = false;
};

}  // namespace f2c_cpp
