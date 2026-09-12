/**
 * @file satellite_plan_confirm_dialog.hpp
 * @brief Office "Save Plan" confirmation: review the ROI and robot pose,
 *        then cache the site imagery as part of saving.
 *
 * Saving a satellite plan in the office is what makes it usable on a roof
 * with no internet, so the imagery download is the save's primary output
 * rather than a separate step. The dialog shows what is about to be saved,
 * derives every download parameter from the ROI, and runs the prefetch in
 * place. The parameters stay reachable behind an Advanced disclosure —
 * defaulted, not removed.
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

class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QToolButton;
class QWidget;

namespace f2c_cpp {

class TileService;

class SatellitePlanConfirmDialog : public QDialog {
    Q_OBJECT

public:
    enum class Outcome { Cancelled, SavedWithImagery, SavedWithoutImagery };

    /**
     * @param job        The plan about to be saved (name, polygon, robot).
     * @param thumbnail  Canvas snapshot framed on the ROI.
     * @param request    Prefetch defaults derived from the ROI; the
     *                   Advanced section edits a copy.
     * @param already_cached  True when this job already has a manifest, so
     *                   the copy can say "refresh" rather than "download".
     */
    SatellitePlanConfirmDialog(TileService* tiles, const Job& job,
                               const QPixmap& thumbnail,
                               const PrefetchRequest& request,
                               bool already_cached,
                               QWidget* parent = nullptr);

    Outcome outcome() const { return outcome_; }
    /** Valid only for SavedWithImagery. */
    TileService::SiteManifest manifest() const { return manifest_; }
    /** Dev screenshot hook: expands the Advanced disclosure. */
    void devSetAdvancedOpen(bool open);

protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void buildUi(const Job& job, const QPixmap& thumbnail);
    void applyStyle();
    void refreshEstimate();
    PrefetchRequest currentRequest() const;
    void onPrimaryClicked();
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
    TileService::SiteManifest manifest_;

    QLabel* estimate_label_ = nullptr;
    QToolButton* advanced_toggle_ = nullptr;
    QWidget* advanced_host_ = nullptr;
    QSpinBox* radius_spin_ = nullptr;
    QSpinBox* max_zoom_spin_ = nullptr;
    QSpinBox* max_age_spin_ = nullptr;
    QCheckBox* clarity_check_ = nullptr;
    QComboBox* wayback_combo_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* status_label_ = nullptr;
    QPushButton* primary_button_ = nullptr;
    QPushButton* without_button_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
};

}  // namespace f2c_cpp
