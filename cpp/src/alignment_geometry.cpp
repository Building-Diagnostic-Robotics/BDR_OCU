#include "alignment_geometry.hpp"

#include <QPolygonF>
#include <QRectF>

#include <algorithm>
#include <cmath>

namespace f2c_cpp {

AlignedCanvas alignedCanvasFor(const Similarity2D& fit,
                               const QSize& image_size,
                               int max_dim) {
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

    // Exactly what the image covers -- see the header on why nothing else
    // may be unioned in here.
    const QPolygonF corners = to_metres.map(
        QPolygonF(QRectF(0, 0, image_size.width(), image_size.height())));
    const QRectF extent = corners.boundingRect();
    if (extent.width() <= 0 || extent.height() <= 0) {
        return canvas;
    }

    // One canvas pixel per image pixel: this canvas exists to turn the image
    // into the robot frame, not to resample it. Finer wastes pixels, coarser
    // throws detail away.
    const double px_per_m = fit.scalePxPerM();
    if (!(px_per_m > 1e-9) || !std::isfinite(px_per_m)) {
        return canvas;
    }
    double mpp = 1.0 / px_per_m;
    int w = static_cast<int>(std::ceil(extent.width() / mpp)) + 1;
    int h = static_cast<int>(std::ceil(extent.height() / mpp)) + 1;
    const int longest = std::max(w, h);
    if (longest > max_dim) {
        mpp *= static_cast<double>(longest) / static_cast<double>(max_dim);
        w = static_cast<int>(std::ceil(extent.width() / mpp)) + 1;
        h = static_cast<int>(std::ceil(extent.height() / mpp)) + 1;
    }
    w = std::clamp(w, 16, max_dim);
    h = std::clamp(h, 16, max_dim);

    // metres -> canvas pixels, row 0 = max northing.
    const QTransform to_canvas(1.0 / mpp, 0.0, 0.0, -1.0 / mpp,
                               -extent.left() / mpp, extent.bottom() / mpp);
    // Qt composes row-vector style: `a * b` applies a first, then b.
    canvas.transform = to_metres * to_canvas;
    canvas.metres_per_px = mpp;
    canvas.width = w;
    canvas.height = h;
    canvas.valid = true;
    return canvas;
}

}  // namespace f2c_cpp
