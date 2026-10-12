/**
 * @file svg_tint.hpp
 * @brief Load an SVG resource and recolor both stroke and fill.
 *
 * Stage 6 uses this for status dots (fill) and icons (stroke or fill).
 * The pixmap is rendered at the screen's device pixel ratio so a 18 px
 * icon stays sharp on the field laptop. Scan Setup and the dashboard keep
 * their own copies; this helper is only for the satellite screen and the
 * scan-parameters panel.
 */

#pragma once

#include <QPixmap>
#include <QString>

namespace f2c_cpp {

/**
 * `device_pixel_ratio <= 0` uses the application's ratio (1 when there is
 * no application). The returned pixmap's device pixel ratio is set, so its
 * logical size is `width` × `height`. An empty color leaves the file's
 * colors alone. `fill="none"` and `stroke="none"` are not rewritten.
 */
QPixmap loadTintedSvg(const QString& resource_path, int width, int height,
                      const QString& color = QString(),
                      qreal device_pixel_ratio = 0.0);

}  // namespace f2c_cpp
