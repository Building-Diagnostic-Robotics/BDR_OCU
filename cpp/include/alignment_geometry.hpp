/**
 * @file alignment_geometry.hpp
 * @brief View geometry for the satellite/point-cloud alignment step.
 *
 * Separated from AutonomyScreen so it can be tested without a widget, a ROS
 * node or a point cloud: these are pure functions of a fit and an image size.
 *
 * PORTED from the parallel OCU under pilot_control/scripts/F2C/cpp (branch
 * `autonomy`, commit a287113). This note is the only intended difference —
 * keep the code identical to that copy.
 *
 * `alignedCanvasFor` has no consumer here yet — Stage 6's live re-projected
 * preview is a later increment, gated on the Figma correspondence frames,
 * which carry no preview pane. It ships now because the ported alignment
 * tests cover it alongside similarity_2d.
 */

#pragma once

#include "similarity_2d.hpp"

#include <QSize>
#include <QTransform>

namespace f2c_cpp {

/**
 * The canvas that holds an image re-projected into the robot frame.
 *
 * Sizing rule: the extent is the image's own footprint in metres and nothing
 * else. That footprint scales as 1/scalePxPerM and `metres_per_px` scales the
 * same way, so width/metres_per_px is SCALE-INVARIANT -- the canvas always
 * comes out about the size of the image, whatever the fit's scale does.
 * Unioning in any fixed metric extent (the point cloud's bounds, say) breaks
 * that invariance: an early fit with a large scale then shrinks the image's
 * metric footprint until the other extent dominates, the canvas becomes
 * mostly padding with the image a speck inside it, and fit-to-view hands the
 * operator that speck to pick on. See canvas_extent_tests.
 */
struct AlignedCanvas {
    /** Original image pixels -> canvas pixels. */
    QTransform transform;
    /** Ground metres per canvas pixel. */
    double metres_per_px = 0.0;
    int width = 0;
    int height = 0;
    bool valid = false;
};

/**
 * Canvas for `image_size` re-projected through `fit` into the robot frame,
 * at one canvas pixel per image pixel so no detail is gained or lost.
 * Row 0 is max northing and robot +X runs along +u, matching how the
 * point-cloud raster is drawn, so neither pane is mirrored against the other.
 *
 * `max_dim` is a backstop against a degenerate transform, not normal
 * operation -- the scale invariance above keeps the canvas near the image's
 * diagonal.
 */
AlignedCanvas alignedCanvasFor(const Similarity2D& fit,
                               const QSize& image_size,
                               int max_dim);

}  // namespace f2c_cpp
