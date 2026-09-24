/**
 * @file satellite_plan_confirm_dialog.hpp
 * @brief Save confirmation: show the area about to be cached, then
 *        download it.
 *
 * Saving a satellite plan is what makes it usable on a roof with no
 * internet, so the imagery download is the save's primary output. The
 * dialog shows the area about to be cached. Radius, zoom, age and layer
 * are the request's defaults; the operator is not asked to choose them.
 *
 * Outcomes:
 *  - SavedWithImagery: prefetch succeeded; `manifest()` is what got cached.
 *  - SavedWithoutImagery: operator chose to keep the plan despite a failed
 *    or unavailable download. The plan is saved uncached and the Dashboard
 *    treats it as not field-ready.
 *  - Cancelled: nothing is saved.
 *
 * Frameless zinc modal like the other Stage 6 dialogs; the caller blurs the
 * parent.
 */

#pragma once

#include "satellite_job_model.hpp"
#include "satellite_site_prefetch.hpp"

#include <QDialog>
#include <QPixmap>

class QLabel;
class QProgressBar;
class QPushButton;
class QWidget;

namespace f2c_cpp {

class TileService;

class SatellitePlanConfirmDialog : public QDialog {
    Q_OBJECT

public:
    enum class Outcome { Cancelled, SavedWithImagery, SavedWithoutImagery };

    /**
     * @param job        The plan about to be saved. The dialog uses the name.
     * @param thumbnail  Snapshot of the map the operator just searched.
     * @param request    Prefetch defaults. The dialog does not edit them.
     * @param already_cached  True when this job already has a manifest, so
     *                   the copy can say "refresh" rather than "download".
     */
    SatellitePlanConfirmDialog(TileService* tiles, const Job& job,
                               const QPixmap& thumbnail,
                               const PrefetchRequest& request,
                               bool already_cached,
                               QWidget* parent = nullptr);

    Outcome outcome() const { return outcome_; }
    /** True when the operator asked to leave after a successful save.
        The dialog still reports SavedWithImagery; the caller decides
        where that lands. */
    bool closeAfterSave() const { return close_after_save_; }
    /** Valid only for SavedWithImagery. */
    TileService::SiteManifest manifest() const { return manifest_; }

protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void buildUi(const Job& job, const QPixmap& thumbnail);
    void applyStyle();
    void refreshEstimate();
    void onPrimaryClicked();
    void onSaveAndCloseClicked();
    void beginDownload();
    void onSaveWithoutImagery();
    void onCancel();
    void onPrefetchFinished(const PrefetchResult& result);
    void setDownloading(bool downloading);

    enum class Phase { Review, Downloading, Failed, Done };

    TileService* tiles_;
    SitePrefetcher* prefetcher_ = nullptr;
    PrefetchRequest base_request_;
    bool already_cached_ = false;
    Phase phase_ = Phase::Review;
    Outcome outcome_ = Outcome::Cancelled;
    bool close_after_save_ = false;
    TileService::SiteManifest manifest_;

    QLabel* estimate_label_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* status_label_ = nullptr;
    QPushButton* primary_button_ = nullptr;
    QPushButton* close_button_ = nullptr;
    QPushButton* without_button_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
};

}  // namespace f2c_cpp
