/**
 * @file swath_preview.hpp
 * @brief Approximate swath lines for the Scan Parameters canvas.
 *
 * The lanes follow the coverage director: axis-aligned in the robot's
 * frame, the first half a width in from the bounding box, then one width
 * apart. Each lane is cut back from the edges by the director's default
 * clearances, and a piece shorter than the director's minimum is dropped.
 * The robot plans the real route; this is only a picture of it.
 */

#pragma once

#include <QLineF>
#include <QPointF>
#include <QVector>

#include <algorithm>

namespace f2c_cpp {

/** Director launch defaults the OCU does not override. */
constexpr double kPreviewRobotRadiusM = 0.30;
constexpr double kPreviewBoundaryClearanceM = 0.25;
constexpr double kPreviewCliffSeedWidthM = 0.50;
constexpr double kPreviewCliffPlanningClearanceM = 0.70;
constexpr double kPreviewMinFragmentWidths = 0.7;
constexpr double kPreviewMinSweepTargetRadii = 1.5;

/** Distance from an edge line to the nearest allowed lane point. */
inline double previewEdgeClearanceM(bool roof_edge) {
    if (roof_edge) {
        // Half the seeded cliff strip lies inside the ROI, then the soft
        // planning inflation (robot radius + cliff planning clearance).
        return 0.5 * kPreviewCliffSeedWidthM + kPreviewRobotRadiusM +
               kPreviewCliffPlanningClearanceM;
    }
    return kPreviewRobotRadiusM + kPreviewBoundaryClearanceM;
}

inline double previewMinFragmentM(double width_m) {
    return std::max(kPreviewMinFragmentWidths * width_m,
                    kPreviewMinSweepTargetRadii * kPreviewRobotRadiusM);
}

struct SwathPreview {
    QVector<QLineF> segments;  // ENU metres, same frame as the input ring
    int passes = 0;            // lanes that still have a piece, not the pieces
};

/**
 * `ring_enu` is metres east/north about the robot marker, the same origin
 * `bodyFromEnu` uses. `roof_edges[i]` marks the edge from vertex i to i+1.
 * Empty when the marker is missing, the ring has fewer than 3 vertices, or
 * the width is not positive.
 */
struct SwathPreviewRequest {
    QVector<QPointF> ring_enu;
    QVector<bool> roof_edges;
    double heading_deg = 0.0;
    bool marker_valid = false;
    double width_m = 0.0;
};

SwathPreview previewSwaths(const SwathPreviewRequest& request);

}  // namespace f2c_cpp
