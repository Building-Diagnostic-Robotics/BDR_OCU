#include "similarity_2d.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <Eigen/Dense>
#include <Eigen/SVD>

namespace f2c_cpp {

QPointF Similarity2D::apply(const QPointF& pcd) const {
    return QPointF(
        a00 * pcd.x() + a01 * pcd.y() + tx,
        a10 * pcd.x() + a11 * pcd.y() + ty);
}

QPointF Similarity2D::inverse(const QPointF& sat) const {
    const double det = a00 * a11 - a01 * a10;
    if (std::abs(det) < 1e-18) {
        return QPointF();
    }
    const double inv_det = 1.0 / det;
    const double dx = sat.x() - tx;
    const double dy = sat.y() - ty;
    return QPointF(
        inv_det * (a11 * dx - a01 * dy),
        inv_det * (-a10 * dx + a00 * dy));
}

double Similarity2D::scalePxPerM() const {
    return std::hypot(a00, a10);
}

namespace {

/**
 * Per-pair weights, normalised so the weighted statistics stay on the same
 * numeric footing as the unweighted ones. Anything malformed (wrong length,
 * non-finite, non-positive) falls back to uniform rather than failing: a
 * caller that has not built a weight vector yet must keep working.
 */
QVector<double> normalizedWeights(const QVector<double>& weights, int n) {
    QVector<double> w(n, 1.0);
    if (weights.size() != n) {
        return w;
    }
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        if (!std::isfinite(weights[i]) || weights[i] <= 0.0) {
            return QVector<double>(n, 1.0);
        }
        sum += weights[i];
    }
    if (!(sum > 0.0) || !std::isfinite(sum)) {
        return QVector<double>(n, 1.0);
    }
    // Mean weight 1, so weighted and unweighted sums are directly comparable.
    const double k = double(n) / sum;
    for (int i = 0; i < n; ++i) {
        w[i] = weights[i] * k;
    }
    return w;
}

SimilarityFit finishFit(const Eigen::Matrix2d& a,
                        const Eigen::Vector2d& t,
                        bool reflected,
                        const QVector<QPointF>& pcd_points,
                        const QVector<QPointF>& sat_points,
                        const QVector<double>& w,
                        int n) {
    SimilarityFit fit;
    fit.transform.a00 = a(0, 0);
    fit.transform.a01 = a(0, 1);
    fit.transform.a10 = a(1, 0);
    fit.transform.a11 = a(1, 1);
    fit.transform.tx = t.x();
    fit.transform.ty = t.y();
    fit.transform.reflected = reflected;
    fit.transform.valid = true;

    double sse_px = 0.0;
    double sse_m = 0.0;
    double wsse_m = 0.0;
    double wsum = 0.0;
    fit.residuals_m.resize(n);
    for (int i = 0; i < n; ++i) {
        const QPointF pred_px = fit.transform.apply(pcd_points[i]);
        const QPointF err_px = pred_px - sat_points[i];
        sse_px += err_px.x() * err_px.x() + err_px.y() * err_px.y();

        const QPointF pred_m = fit.transform.inverse(sat_points[i]);
        const QPointF err_m = pred_m - pcd_points[i];
        const double sq_m = err_m.x() * err_m.x() + err_m.y() * err_m.y();
        sse_m += sq_m;
        wsse_m += w[i] * sq_m;
        wsum += w[i];
        fit.residuals_m[i] = std::sqrt(sq_m);
    }
    fit.rmse_px = std::sqrt(sse_px / static_cast<double>(n));
    fit.rmse_m = std::sqrt(sse_m / static_cast<double>(n));
    fit.weighted_rmse_m =
        (wsum > 0.0) ? std::sqrt(wsse_m / wsum) : fit.rmse_m;
    return fit;
}

std::optional<SimilarityFit> umeyamaOnce(
    const QVector<QPointF>& pcd_points,
    const QVector<QPointF>& sat_points,
    const QVector<double>& w,
    int n,
    bool allow_reflection) {
    double wsum = 0.0;
    Eigen::Vector2d mu_src = Eigen::Vector2d::Zero();
    Eigen::Vector2d mu_dst = Eigen::Vector2d::Zero();
    for (int i = 0; i < n; ++i) {
        mu_src += w[i] * Eigen::Vector2d(pcd_points[i].x(), pcd_points[i].y());
        mu_dst += w[i] * Eigen::Vector2d(sat_points[i].x(), sat_points[i].y());
        wsum += w[i];
    }
    if (!(wsum > 0.0)) {
        return std::nullopt;
    }
    mu_src /= wsum;
    mu_dst /= wsum;

    Eigen::Matrix2d cov = Eigen::Matrix2d::Zero();
    double src_var = 0.0;
    for (int i = 0; i < n; ++i) {
        const Eigen::Vector2d src(pcd_points[i].x() - mu_src.x(),
                                  pcd_points[i].y() - mu_src.y());
        const Eigen::Vector2d dst(sat_points[i].x() - mu_dst.x(),
                                  sat_points[i].y() - mu_dst.y());
        cov += w[i] * dst * src.transpose();
        src_var += w[i] * src.squaredNorm();
    }
    cov /= wsum;
    src_var /= wsum;
    if (src_var < 1e-12) {
        return std::nullopt;
    }

    Eigen::JacobiSVD<Eigen::Matrix2d> svd(
        cov, Eigen::ComputeFullU | Eigen::ComputeFullV);
    Eigen::Matrix2d u = svd.matrixU();
    Eigen::Matrix2d v = svd.matrixV();
    Eigen::Matrix2d d = Eigen::Matrix2d::Identity();
    const bool svd_reflects = (u * v.transpose()).determinant() < 0.0;
    if (svd_reflects && !allow_reflection) {
        d(1, 1) = -1.0;
    }
    const Eigen::Matrix2d r = u * d * v.transpose();
    const double scale = (svd.singularValues().asDiagonal() * d).trace() / src_var;
    if (!(scale > 1e-9) || !std::isfinite(scale)) {
        return std::nullopt;
    }
    const Eigen::Vector2d t = mu_dst - scale * r * mu_src;
    const bool reflected = r.determinant() < 0.0;
    return finishFit(scale * r, t, reflected, pcd_points, sat_points, w, n);
}


/**
 * Hat-matrix leverage for the weighted similarity fit.
 *
 * Written as a complex regression w = a*conj(z) + b, the design is an
 * intercept plus one predictor, so the leverage collapses to the familiar
 * simple-regression form and needs only squared distances from the weighted
 * centroid. Sums to 2 -- two complex parameters, i.e. the four real DOF of
 * the similarity spread over 2n real equations.
 */
QVector<double> leverages(const QVector<QPointF>& pcd,
                          const QVector<double>& w, int n) {
    QVector<double> h(n, 0.0);
    double wsum = 0.0;
    QPointF mu(0.0, 0.0);
    for (int i = 0; i < n; ++i) {
        wsum += w[i];
        mu += w[i] * pcd[i];
    }
    if (!(wsum > 0.0)) {
        return h;
    }
    mu /= wsum;
    double spread = 0.0;
    for (int i = 0; i < n; ++i) {
        const QPointF d = pcd[i] - mu;
        spread += w[i] * (d.x() * d.x() + d.y() * d.y());
    }
    for (int i = 0; i < n; ++i) {
        const QPointF d = pcd[i] - mu;
        const double d2 = d.x() * d.x() + d.y() * d.y();
        h[i] = w[i] * (1.0 / wsum + ((spread > 0.0) ? (d2 / spread) : 0.0));
    }
    return h;
}

}  // namespace

std::optional<SimilarityFit> estimateSimilarity2D(
    const QVector<QPointF>& pcd_points,
    const QVector<QPointF>& sat_points,
    const QVector<double>& weights) {
    const int n = std::min(pcd_points.size(), sat_points.size());
    if (n < 2) {
        return std::nullopt;
    }
    const QVector<double> w = normalizedWeights(weights, n);

    // Compare proper rotation vs reflection. Image Y-down vs robot Y-up
    // needs a reflection; forcing det(R)=+1 is what made earlier fits drift.
    const auto with_reflect = umeyamaOnce(pcd_points, sat_points, w, n, true);
    const auto no_reflect = umeyamaOnce(pcd_points, sat_points, w, n, false);
    if (with_reflect && no_reflect) {
        return (with_reflect->weighted_rmse_m <= no_reflect->weighted_rmse_m)
                   ? with_reflect
                   : no_reflect;
    }
    if (with_reflect) {
        return with_reflect;
    }
    return no_reflect;
}

std::optional<RobustFit> fitSimilarityRobust(
    const QVector<QPointF>& pcd_points,
    const QVector<QPointF>& sat_points,
    const QVector<double>& sigma_m,
    const RobustFitOptions& options) {
    const int n = std::min(pcd_points.size(), sat_points.size());
    if (n < 2 || sigma_m.size() != n) {
        return std::nullopt;
    }
    // A non-finite or non-positive sigma would divide the fit by zero. Fall
    // back to the largest sane sigma present rather than inventing a scale.
    double fallback = 0.0;
    for (int i = 0; i < n; ++i) {
        if (std::isfinite(sigma_m[i]) && sigma_m[i] > 0.0) {
            fallback = std::max(fallback, sigma_m[i]);
        }
    }
    if (!(fallback > 0.0)) {
        fallback = 1.0;
    }
    QVector<double> sigma(n);
    for (int i = 0; i < n; ++i) {
        sigma[i] = (std::isfinite(sigma_m[i]) && sigma_m[i] > 0.0)
                       ? sigma_m[i]
                       : fallback;
    }

    const double k = (options.huber_k > 0.0) ? options.huber_k : 2.0;
    // Leverage reaches 1 as the fit becomes exact (n = 2), which would make
    // the correction singular. Floor it; the outlier test is gated at n >= 4
    // anyway, where the redundancy makes studentisation meaningful.
    constexpr double kMinUnleveraged = 1e-3;

    RobustFit out;
    QVector<double> rho(n, 1.0);
    QVector<double> w(n, 1.0);
    std::optional<SimilarityFit> fit;
    QVector<double> h;

    for (int iter = 0; iter < std::max(1, options.max_iterations); ++iter) {
        for (int i = 0; i < n; ++i) {
            w[i] = rho[i] / (sigma[i] * sigma[i]);
        }
        fit = estimateSimilarity2D(pcd_points, sat_points, w);
        if (!fit || !fit->transform.valid) {
            return std::nullopt;
        }
        h = leverages(pcd_points, w, n);
        double max_change = 0.0;
        for (int i = 0; i < n; ++i) {
            const double denom =
                sigma[i] * std::sqrt(std::max(kMinUnleveraged, 1.0 - h[i]));
            const double u = fit->residuals_m[i] / denom;
            // Huber: quadratic within k sigma, linear beyond, so a blunder's
            // pull grows no further no matter how wrong it is.
            const double next = (u <= k) ? 1.0 : (k / u);
            max_change = std::max(max_change, std::abs(next - rho[i]));
            rho[i] = next;
        }
        out.iterations = iter + 1;
        if (max_change < options.tolerance) {
            out.converged = true;
            break;
        }
    }

    // Final pass with the converged weights, so the reported fit is the one
    // the weights describe.
    for (int i = 0; i < n; ++i) {
        w[i] = rho[i] / (sigma[i] * sigma[i]);
    }
    fit = estimateSimilarity2D(pcd_points, sat_points, w);
    if (!fit || !fit->transform.valid) {
        return std::nullopt;
    }
    h = leverages(pcd_points, w, n);

    out.fit = *fit;
    out.leverage = h;
    out.robust_weight = rho;
    out.studentized.resize(n);
    out.min_detectable_m.resize(n);
    // Standard 2p/n leverage rule; p = 2 complex parameters, and the
    // leverages sum to 2, so this is twice the mean.
    const double leverage_limit = 4.0 / static_cast<double>(n);
    for (int i = 0; i < n; ++i) {
        const double unleveraged = std::max(kMinUnleveraged, 1.0 - h[i]);
        const double denom = sigma[i] * std::sqrt(unleveraged);
        out.studentized[i] = fit->residuals_m[i] / denom;
        // A blunder on pair i is absorbed by the fit in proportion to its
        // leverage, leaving residual ~ B(1 - h), so it only becomes visible
        // once B * sqrt(1 - h) / sigma clears the threshold.
        out.min_detectable_m[i] =
            sigma[i] * options.outlier_sigma / std::sqrt(unleveraged);
        // Below 4 pairs there are fewer than 4 redundant equations and the
        // studentised residual is not a test of anything.
        if (n >= 4 && out.studentized[i] > options.outlier_sigma) {
            out.outliers.push_back(i);
        }
        if (n >= 4 && h[i] > leverage_limit) {
            out.weakly_checked.push_back(i);
        }
    }
    return out;
}

OutlierReport leaveOneOutOutlier(
    const QVector<QPointF>& pcd_points,
    const QVector<QPointF>& sat_points,
    const QVector<double>& weights,
    double flag_improvement_frac) {
    OutlierReport report;
    const int n = std::min(pcd_points.size(), sat_points.size());
    // Dropping one must leave an over-determined set: at n = 3 the remaining
    // 2 pairs fit exactly and every candidate scores a perfect 0.
    if (n < 4) {
        return report;
    }
    const bool have_weights = (weights.size() == n);

    const auto full = estimateSimilarity2D(pcd_points, sat_points, weights);
    if (!full || !full->transform.valid) {
        return report;
    }
    report.full_rmse_m = full->weighted_rmse_m;
    report.rmse_without_m.resize(n);

    double best = std::numeric_limits<double>::max();
    for (int skip = 0; skip < n; ++skip) {
        QVector<QPointF> pcd;
        QVector<QPointF> sat;
        QVector<double> w;
        pcd.reserve(n - 1);
        sat.reserve(n - 1);
        if (have_weights) {
            w.reserve(n - 1);
        }
        for (int i = 0; i < n; ++i) {
            if (i == skip) {
                continue;
            }
            pcd.push_back(pcd_points[i]);
            sat.push_back(sat_points[i]);
            if (have_weights) {
                w.push_back(weights[i]);
            }
        }
        const auto sub = estimateSimilarity2D(pcd, sat, w);
        if (!sub || !sub->transform.valid) {
            report.rmse_without_m[skip] = std::numeric_limits<double>::max();
            continue;
        }
        report.rmse_without_m[skip] = sub->weighted_rmse_m;
        if (sub->weighted_rmse_m < best) {
            best = sub->weighted_rmse_m;
            report.worst_index = skip;
        }
    }
    if (report.worst_index < 0) {
        return report;
    }
    report.valid = true;
    report.best_rmse_without_m = best;
    // A loose-but-honest pick set improves only marginally when any one pair
    // is dropped. A genuine outlier improves it a lot, and only for itself.
    report.flagged =
        (report.full_rmse_m > 0.0) &&
        (best < flag_improvement_frac * report.full_rmse_m);
    return report;
}

}  // namespace f2c_cpp
