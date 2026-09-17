#include "alignment_geometry.hpp"

#include <QPolygonF>
#include <QRectF>

#include <algorithm>
#include <cmath>

namespace f2c_cpp {

AlignedCanvas alignedCanvasFor(const Similarity2D& fit,
                               const QSize& image_size,
                               int max_dim,
                               const QRectF& keep_image_px) {
    AlignedCanvas canvas;
    if (!fit.valid || image_size.isEmpty() || max_dim < 16) {
        return canvas;
    }
    // robot metres -> original image pixels, as the fit is defined.
    // QTransform::map is x' = m11*x + m21*y + dx, y' = m12*x + m22*y + dy.
    const QTransform to_image(fit.a00, fit.a10, fit.a01, fit.a11, fit.tx,
                              fit.ty);
    bool invertible = false;
    const QTransform to_metres = to_image.inverted(&invertible);
    if (!invertible) {
        return canvas;
    }

    // One canvas pixel per image pixel: this canvas exists to turn the image
    // into the robot frame, not to resample it. Finer wastes pixels, coarser
    // throws detail away. Needed before the crop, which insets by whole canvas
    // pixels.
    const double px_per_m = fit.scalePxPerM();
    if (!(px_per_m > 1e-9) || !std::isfinite(px_per_m)) {
        return canvas;
    }

    // Exactly what the image covers -- see the header on why no fixed metric
    // extent may be unioned in here.
    const QPolygonF corners = to_metres.map(
        QPolygonF(QRectF(0, 0, image_size.width(), image_size.height())));
    const QRectF extent = corners.boundingRect();
    if (extent.width() <= 0 || extent.height() <= 0) {
        return canvas;
    }

    // The bounding box of a TURNED image is mostly not image: its corners are
    // empty. Shrink it about its centre until it fits INSIDE the footprint, so
    // every canvas pixel lands on real imagery instead of on padding.
    //
    // An axis-aligned w x h centred on the footprint fits iff
    // w*c + h*s <= side_u and w*s + h*c <= side_v, where c and s are the
    // |cos| / |sin| the bounding box was itself built from. Scaling the box by
    // the smaller of the two ratios therefore satisfies both by construction:
    // one expression, no special cases, a no-op at 0 and 90 degrees and exactly
    // the largest inscribed rectangle at 45.
    const QPointF origin_m = to_metres.map(QPointF(0.0, 0.0));
    const QPointF edge_u =
        to_metres.map(QPointF(image_size.width(), 0.0)) - origin_m;
    const QPointF edge_v =
        to_metres.map(QPointF(0.0, image_size.height())) - origin_m;
    const double side_u = std::hypot(edge_u.x(), edge_u.y());
    const double side_v = std::hypot(edge_v.x(), edge_v.y());
    QRectF want = extent;
    if (side_u > 0.0 && side_v > 0.0) {
        const double c = std::abs(edge_u.x()) / side_u;
        const double s = std::abs(edge_u.y()) / side_u;
        const double lim_u = extent.width() * c + extent.height() * s;
        const double lim_v = extent.width() * s + extent.height() * c;
        const double t = std::min({(lim_u > 0.0) ? side_u / lim_u : 1.0,
                                   (lim_v > 0.0) ? side_v / lim_v : 1.0, 1.0});
        want.setSize(extent.size() * t);
        want.moveCenter(extent.center());
        // Tolerance, not `t < 1.0`: at a quarter turn cos() is 1e-17 rather than
        // 0, so an exact-fit crop otherwise reads as having bitten and the inset
        // shaves an unturned view for nothing.
        if (t < 1.0 - 1e-9) {
            // The crop lands tangent to the footprint, and the sizing below
            // rounds the extent UP to whole pixels, so the last row and column
            // would sit just outside the imagery. Give back what that rounding
            // takes. Skipped when the crop did nothing, so an unturned fit is
            // byte-identical to sizing straight from the bounding box.
            const double inset = 2.0 / px_per_m;
            want.adjust(inset, inset, -inset, -inset);
        }
    }

    // Never crop a pick off the imagery: markers are drawn unclipped, so a pair
    // placed in a corner before the view started turning would otherwise be left
    // as a numbered pin floating on blank matte. Pick positions are ORIGINAL
    // image pixels, so unioning them scales with the fit exactly as the
    // footprint does -- this is not the fixed-metric union the rule above bans.
    if (!keep_image_px.isEmpty()) {
        want = want.united(to_metres.mapRect(keep_image_px)).intersected(extent);
    }
    if (want.width() <= 0 || want.height() <= 0) {
        want = extent;
    }

    double mpp = 1.0 / px_per_m;
    int w = static_cast<int>(std::ceil(want.width() / mpp)) + 1;
    int h = static_cast<int>(std::ceil(want.height() / mpp)) + 1;
    const int longest = std::max(w, h);
    if (longest > max_dim) {
        mpp *= static_cast<double>(longest) / static_cast<double>(max_dim);
        w = static_cast<int>(std::ceil(want.width() / mpp)) + 1;
        h = static_cast<int>(std::ceil(want.height() / mpp)) + 1;
    }
    w = std::clamp(w, 16, max_dim);
    h = std::clamp(h, 16, max_dim);

    // metres -> canvas pixels, row 0 = max northing.
    const QTransform to_canvas(1.0 / mpp, 0.0, 0.0, -1.0 / mpp,
                               -want.left() / mpp, want.bottom() / mpp);
    // Qt composes row-vector style: `a * b` applies a first, then b.
    canvas.transform = to_metres * to_canvas;
    canvas.metres_per_px = mpp;
    canvas.width = w;
    canvas.height = h;
    canvas.valid = true;
    return canvas;
}

}  // namespace f2c_cpp
