/**
 * @file swath_preview.hpp
 * @brief Approximate swath lines for the Scan Parameters canvas.
 *
 * The lanes follow the coverage director: axis-aligned in the robot's
 * frame, the first half a width in from the bounding box, then one width
 * apart. Each lane is cut back from the edges by the director's default
 * clearances, and a piece shorter than 0.60 m is dropped. That floor is
 * the director's fixed minimum (2 × robot radius), not a fraction of the
 * lane width — at 7 ft a width-scaled floor hides pieces the robot drives.
 * The robot plans the real route; this is only a picture of it.
 */

#pragma once

#include <QLineF>
#include <QPointF>
#include <QVector>

namespace f2c_cpp {

/** Director launch defaults the OCU does not override. */
constexpr double kPreviewRobotRadiusM = 0.30;
constexpr double kPreviewBoundaryClearanceM = 0.25;
constexpr double kPreviewCliffSeedWidthM = 0.50;
constexpr double kPreviewCliffPlanningClearanceM = 0.70;
/** Director `MIN_FRAGMENT_RADII`. The fragment floor is this times the radius. */
constexpr double kPreviewMinFragmentRadii = 2.0;

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

/** Fixed 2 × robot radius. `width_m` is unused; the director does not scale this. */
inline double previewMinFragmentM(double /*width_m*/) {
    return kPreviewMinFragmentRadii * kPreviewRobotRadiusM;
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
