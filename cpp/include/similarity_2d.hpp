/**
 * @file similarity_2d.hpp
 * @brief 2D similarity transform (scale, rotation, optional reflection, translation).
 *
 * Maps PCD / robot_init meters -> satellite image pixels.
 *
 * PORTED from the parallel OCU under pilot_control/scripts/F2C/cpp (branch
 * `autonomy`, commit a287113). This note is the only intended difference —
 * keep the code identical to that copy. Alignment work still lands there, so
 * the next port stays a three-way diff only as long as neither side edits in
 * place. Fix bugs in both or in neither.
 */

#pragma once

#include <QPointF>
#include <QVector>
#include <optional>

namespace f2c_cpp {

struct Similarity2D {
    // Linear part: scale * R, or scale * R * diag(1, -1) when reflected.
    double a00 = 1.0;
    double a01 = 0.0;
    double a10 = 0.0;
    double a11 = 1.0;
    double tx = 0.0;
    double ty = 0.0;
    bool reflected = false;
    bool valid = false;

    QPointF apply(const QPointF& pcd) const;
    QPointF inverse(const QPointF& sat) const;
    double scalePxPerM() const;
};

struct SimilarityFit {
    Similarity2D transform;
    double rmse_px = 0.0;
    double rmse_m = 0.0;
    /**
     * Precision-weighted RMSE in the PCD frame. Equal to `rmse_m` when the
     * weights are uniform, so an unweighted caller sees no change. This is
     * the number the solver compares its reflection branches on and the one
     * an acceptance gate should read: it is the residual measured against
     * what each pick was actually worth.
     */
    double weighted_rmse_m = 0.0;
    /**
     * Per-pair residual in PCD meters, in the order the points were passed.
     * Unweighted on purpose — this is what the operator is shown, and a pick
     * being imprecise should not make its miss look smaller than it is.
     */
    QVector<double> residuals_m;
};

/**
 * Umeyama 2D similarity, optionally precision-weighted.
 *
 * `weights` are 1/sigma^2 per pair in consistent units; pass an empty vector
 * (the default) for the uniform case. A size mismatch is treated as uniform
 * rather than as an error, so a caller that has not built weights yet keeps
 * working.
 *
 * Reflection is deliberately left free. Robot XY is right-handed and image
 * XY is Y-down, so the correct fit IS orientation-reversing; both branches
 * are solved and the lower weighted RMSE wins.
 */
std::optional<SimilarityFit> estimateSimilarity2D(
    const QVector<QPointF>& pcd_points,
    const QVector<QPointF>& sat_points,
    const QVector<double>& weights = QVector<double>());

/**
 * Leave-one-out refit, used to find the pair that is inconsistent with the
 * rest rather than merely the one with the largest residual (a single bad
 * pick drags the fit toward itself, which shrinks its own residual and
 * inflates everyone else's).
 *
 * Needs at least 4 pairs: dropping one has to leave a set that is still
 * over-determined, and 2 pairs fit a similarity exactly with zero residual.
 */
struct OutlierReport {
    bool valid = false;
    /** Weighted RMSE (m) of the fit over all pairs. */
    double full_rmse_m = 0.0;
    /** Weighted RMSE (m) of the fit with pair i removed, indexed as passed. */
    QVector<double> rmse_without_m;
    /** Pair whose removal improves the fit most; -1 when none was run. */
    int worst_index = -1;
    /** `rmse_without_m[worst_index]`. */
    double best_rmse_without_m = 0.0;
    /**
     * True when removing `worst_index` cuts the weighted RMSE by more than
     * the flag fraction — i.e. this pair disagrees with the others rather
     * than the whole set simply being loose.
     */
    bool flagged = false;
};

/**
 * Robust fit settings. Defaults are tuned for the 5-10 pair regime this
 * screen works in, where there is too little data to estimate a noise scale
 * from the residuals (a MAD over 5 points is meaningless) -- so the absolute
 * per-pick sigma from the pick-precision model is used instead.
 */
struct RobustFitOptions {
    /** Huber threshold, in units of the pair's own sigma. */
    double huber_k = 2.0;
    /**
     * Studentised residual above which a pair is called a blunder rather
     * than noise. The statistic is a 2-D residual magnitude over its own
     * sigma, so under the null its square is chi-squared with 2 DOF and
     * P(d > 3.5) = exp(-3.5^2/2) ~ 0.002 per pair.
     */
    double outlier_sigma = 3.5;
    int max_iterations = 8;
    double tolerance = 1e-6;
};

struct RobustFit {
    SimilarityFit fit;
    /**
     * Hat-matrix leverage per pair, summing to 2 (the model is two complex
     * parameters). A pair far from the centroid has high leverage: the fit
     * chases it, which SUPPRESSES its raw residual. Ranking on raw residual
     * therefore makes the most valuable -- and most damaging when wrong --
     * pick look innocent, which is what this corrects for.
     */
    QVector<double> leverage;
    /** |r_i| / (sigma_i * sqrt(1 - h_i)). Leverage-corrected, absolute. */
    QVector<double> studentized;
    /** Huber factor finally applied, in (0, 1]. 1 means untouched. */
    QVector<double> robust_weight;
    /** Indices whose studentised residual exceeds `outlier_sigma`. */
    QVector<int> outliers;
    /**
     * Indices with so much leverage that the fit passes through them almost
     * regardless of whether they are right (h_i > 2p/n, the standard rule,
     * p = 2 here). Their residual is suppressed towards zero, so NO
     * residual-based test can check them -- reporting them as clean would be
     * an all-clear the data does not support. `minDetectableM` says how
     * large a blunder would have to be before it became visible.
     */
    QVector<int> weakly_checked;
    /**
     * Smallest blunder the studentised test could detect on pair i, in PCD
     * metres: sigma_i * outlier_sigma / sqrt(1 - h_i). Grows without bound
     * as leverage approaches 1.
     */
    QVector<double> min_detectable_m;
    int iterations = 0;
    bool converged = false;
};

/**
 * Iteratively reweighted least squares on top of the precision-weighted
 * solve: fit, measure each pair against its own expected noise, downweight
 * what disagrees, refit.
 *
 * Schweppe-type -- the residual is divided by sqrt(1 - h_i) before the Huber
 * test, so leverage only matters for a pair that is ALSO a residual outlier.
 * A plain Mallows-type scheme would downweight distant pairs on leverage
 * alone, which here would shrink the very baseline that sets heading
 * precision. Nothing is ever removed: a marginal pair keeps partial weight
 * and keeps contributing its spread.
 *
 * `sigma_m` is the absolute 1-sigma pick uncertainty per pair, per
 * coordinate, in PCD metres -- the same units residuals are measured in.
 */
std::optional<RobustFit> fitSimilarityRobust(
    const QVector<QPointF>& pcd_points,
    const QVector<QPointF>& sat_points,
    const QVector<double>& sigma_m,
    const RobustFitOptions& options = RobustFitOptions());

OutlierReport leaveOneOutOutlier(
    const QVector<QPointF>& pcd_points,
    const QVector<QPointF>& sat_points,
    const QVector<double>& weights = QVector<double>(),
    double flag_improvement_frac = 0.5);

}  // namespace f2c_cpp
