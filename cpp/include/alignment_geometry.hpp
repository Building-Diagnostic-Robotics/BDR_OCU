/**
 * @file alignment_geometry.hpp
 * @brief View geometry for the satellite/point-cloud alignment step.
 *
 * Separated from AutonomyScreen so it can be tested without a widget, a ROS
 * node or a point cloud: these are pure functions of a fit and an image size.
 *
 * PORTED from the parallel OCU under pilot_control/scripts/F2C/cpp (branch
 * `autonomy`, commit a287113).
 *
 * DIVERGED from that copy on purpose: `alignedCanvasFor` here crops the canvas
 * to the largest axis-aligned rectangle inside the turned footprint (see its
 * sizing rule below) and takes a `keep_image_px` argument, where upstream still
 * sizes to the full bounding box with empty corners. The crop is what lets the
 * canvas stay at one pixel per image pixel on a large site instead of being
 * coarsened by `max_dim`, which costs pick precision. This is NOT yet ported
 * back — until it is, treat this file as a two-way diff, not a copy, and do not
 * "restore" it to upstream.
 */

#pragma once

#include "similarity_2d.hpp"

#include <QRectF>
#include <QSize>
#include <QTransform>

namespace f2c_cpp {

/**
 * The canvas that holds an image re-projected into the robot frame.
 *
 * Sizing rule: the extent derives from the image's own footprint in metres and
 * nothing else. That footprint scales as 1/scalePxPerM and `metres_per_px`
 * scales the same way, so width/metres_per_px is SCALE-INVARIANT -- the canvas
 * always comes out about the size of the image, whatever the fit's scale does.
 * Unioning in any fixed metric extent (the point cloud's bounds, say) breaks
 * that invariance: an early fit with a large scale then shrinks the image's
 * metric footprint until the other extent dominates, the canvas becomes
 * mostly padding with the image a speck inside it, and fit-to-view hands the
 * operator that speck to pick on. See canvas_extent_tests.
 *
 * Within that footprint the extent is CROPPED to the largest axis-aligned
 * rectangle that fits inside it, because the bounding box of a turned image is
 * mostly empty corners. That is a no-op on an unturned fit and tightest at 45
 * degrees, and it keeps the canvas smaller than the image rather than up to 1.4x
 * larger -- which is what stops `max_dim` from binding and coarsening the
 * imagery, since a coarser canvas is one the operator cannot pick as finely on.
 *
 * `keep_image_px` is unioned back in after the crop: pick positions, in ORIGINAL
 * image pixels, that must stay on the canvas. Markers are drawn unclipped, so a
 * pick cropped away becomes a numbered pin on blank matte, which is worse than
 * an empty corner. Being image-relative it scales with the fit exactly as the
 * footprint does, so it is NOT the fixed-metric union banned above. It can push
 * the extent back out and leave some corner empty again; that is fine, the
 * caller fills the canvas transparent and the pane's matte shows through.
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
 * diagonal, and the crop keeps it under that. If it ever binds, the crop has
 * regressed.
 */
AlignedCanvas alignedCanvasFor(const Similarity2D& fit,
                               const QSize& image_size,
                               int max_dim,
                               const QRectF& keep_image_px = QRectF());

}  // namespace f2c_cpp
