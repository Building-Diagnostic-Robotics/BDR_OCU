#include "similarity_2d.hpp"

#include <algorithm>
#include <cmath>

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

SimilarityFit finishFit(const Eigen::Matrix2d& a,
                        const Eigen::Vector2d& t,
                        bool reflected,
                        const QVector<QPointF>& pcd_points,
                        const QVector<QPointF>& sat_points,
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
    for (int i = 0; i < n; ++i) {
        const QPointF pred_px = fit.transform.apply(pcd_points[i]);
        const QPointF err_px = pred_px - sat_points[i];
        sse_px += err_px.x() * err_px.x() + err_px.y() * err_px.y();

        const QPointF pred_m = fit.transform.inverse(sat_points[i]);
        const QPointF err_m = pred_m - pcd_points[i];
        sse_m += err_m.x() * err_m.x() + err_m.y() * err_m.y();
    }
    fit.rmse_px = std::sqrt(sse_px / static_cast<double>(n));
    fit.rmse_m = std::sqrt(sse_m / static_cast<double>(n));
    return fit;
}

std::optional<SimilarityFit> umeyamaOnce(
    const QVector<QPointF>& pcd_points,
    const QVector<QPointF>& sat_points,
    int n,
    bool allow_reflection) {
    Eigen::Vector2d mu_src = Eigen::Vector2d::Zero();
    Eigen::Vector2d mu_dst = Eigen::Vector2d::Zero();
    for (int i = 0; i < n; ++i) {
        mu_src += Eigen::Vector2d(pcd_points[i].x(), pcd_points[i].y());
        mu_dst += Eigen::Vector2d(sat_points[i].x(), sat_points[i].y());
    }
    mu_src /= static_cast<double>(n);
    mu_dst /= static_cast<double>(n);

    Eigen::Matrix2d cov = Eigen::Matrix2d::Zero();
    double src_var = 0.0;
    for (int i = 0; i < n; ++i) {
        const Eigen::Vector2d src(pcd_points[i].x() - mu_src.x(),
                                  pcd_points[i].y() - mu_src.y());
        const Eigen::Vector2d dst(sat_points[i].x() - mu_dst.x(),
                                  sat_points[i].y() - mu_dst.y());
        cov += dst * src.transpose();
        src_var += src.squaredNorm();
    }
    cov /= static_cast<double>(n);
    src_var /= static_cast<double>(n);
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
    const double scale =
        (svd.singularValues().asDiagonal() * d).trace() / src_var;
    if (!(scale > 1e-9) || !std::isfinite(scale)) {
        return std::nullopt;
    }
    const Eigen::Vector2d t = mu_dst - scale * r * mu_src;
    const bool reflected = r.determinant() < 0.0;
    return finishFit(scale * r, t, reflected, pcd_points, sat_points, n);
}

}  // namespace

std::optional<SimilarityFit> estimateSimilarity2D(
    const QVector<QPointF>& pcd_points,
    const QVector<QPointF>& sat_points) {
    const int n = std::min(pcd_points.size(), sat_points.size());
    if (n < 2) {
        return std::nullopt;
    }

    const auto with_reflect = umeyamaOnce(pcd_points, sat_points, n, true);
    const auto no_reflect = umeyamaOnce(pcd_points, sat_points, n, false);
    if (with_reflect && no_reflect) {
        return (with_reflect->rmse_m <= no_reflect->rmse_m) ? with_reflect
                                                           : no_reflect;
    }
    if (with_reflect) {
        return with_reflect;
    }
    return no_reflect;
}

}  // namespace f2c_cpp
