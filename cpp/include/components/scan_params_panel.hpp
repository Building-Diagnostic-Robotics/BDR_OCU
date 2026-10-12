/**
 * @file scan_params_panel.hpp
 * @brief Stage 6 Scan Parameters card: Detailed / Standard / Quick / Custom.
 *
 * Custom expands in place to a speed row and a vertical lane-width slider.
 * The widths and speeds come only from ScanParams. Display goes through the
 * units layer; speed is a name (Slow / Medium / Fast), never a number.
 */

#pragma once

#include "satellite_job_model.hpp"

#include <QWidget>

namespace f2c_cpp {

class ScanParamsPanel : public QWidget {
    Q_OBJECT

public:
    explicit ScanParamsPanel(QWidget* parent = nullptr);
    ~ScanParamsPanel() override;

    ScanParams params() const { return params_; }
    /** Programmatic. Does not emit paramsChanged. */
    void setParams(const ScanParams& params);
    /**
     * Forget Custom's remembered width and speed. The next time the operator
     * opens Custom it seeds from whichever preset is showing. Call this when
     * a plan is loaded or a new plan starts; tapping a preset and coming
     * back must keep the values until then.
     */
    void resetCustomMemory();
    /** Locked dims the cards and says why in the blurb, which stays full contrast. */
    void setEditable(bool editable);
    void setDarkMode(bool dark_mode);

    /** "3 ft" or "3.5 ft" on the foot grid; metres when the units layer says so. */
    static QString laneWidthText(double meters);

signals:
    void paramsChanged(const f2c_cpp::ScanParams& params);

private:
    void selectPreset(ScanPreset preset);
    void setCustomSpeed(double mps);
    void setCustomHalfFeet(int half_feet);
    void refresh();
    void restyle();

    ScanParams params_;
    bool editable_ = true;
    bool applying_ = false;
    bool dark_mode_ = false;
    bool custom_set_ = false;
    double custom_width_m_ = ScanParams::kWidthDefaultM;
    double custom_speed_mps_ = ScanParams::kSpeedDefaultMps;

    class PanelPrivate;
    PanelPrivate* ui_ = nullptr;
};

}  // namespace f2c_cpp
