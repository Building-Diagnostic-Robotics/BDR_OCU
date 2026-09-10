/**
 * @file similarity_2d.hpp
 * @brief 2D similarity transform (scale, rotation, optional reflection, translation).
 *
 * Maps PCD / robot_init meters -> satellite image pixels.
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
};

std::optional<SimilarityFit> estimateSimilarity2D(
    const QVector<QPointF>& pcd_points,
    const QVector<QPointF>& sat_points);

}  // namespace f2c_cpp
