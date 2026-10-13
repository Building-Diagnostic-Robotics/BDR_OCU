#include "svg_tint.hpp"

#include <QFile>
#include <QGuiApplication>
#include <QPainter>
#include <QScreen>
#include <QSvgRenderer>

namespace f2c_cpp {

QPixmap loadTintedSvg(const QString& resource_path, int width, int height,
                      const QString& color, qreal device_pixel_ratio) {
    QFile file(resource_path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QPixmap();
    }
    QByteArray data = file.readAll();
    file.close();
    if (!color.isEmpty()) {
        for (const QByteArray needle : {QByteArrayLiteral("stroke=\""),
                                        QByteArrayLiteral("fill=\"")}) {
            int index = data.indexOf(needle);
            while (index >= 0) {
                const int value_start = index + needle.size();
                const int value_end = data.indexOf('"', value_start);
                if (value_end <= value_start) {
                    break;
                }
                const QByteArray value =
                    data.mid(value_start, value_end - value_start);
                if (value != "none") {
                    data = data.left(value_start) + color.toUtf8() +
                           data.mid(value_end);
                }
                index = data.indexOf(needle, value_start);
            }
        }
    }
    QSvgRenderer renderer(data);
    if (!renderer.isValid() || width <= 0 || height <= 0) {
        return QPixmap();
    }
    qreal dpr = device_pixel_ratio;
    if (dpr <= 0.0) {
        dpr = 1.0;
        const auto* gui = qobject_cast<const QGuiApplication*>(
            QCoreApplication::instance());
        if (gui && gui->primaryScreen()) {
            const qreal screen_dpr = gui->primaryScreen()->devicePixelRatio();
            if (screen_dpr > 0.0) {
                dpr = screen_dpr;
            }
        }
    }
    if (dpr < 1.0) {
        dpr = 1.0;
    }
    const int pixel_w = qRound(width * dpr);
    const int pixel_h = qRound(height * dpr);
    QPixmap pixmap(pixel_w, pixel_h);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    // Logical pixels. Qt scales these by the pixmap's device pixel ratio,
    // so the drawing fills the sharp pixmap. A physical-pixel rect is
    // scaled again and the icon is clipped to its top-left quarter.
    renderer.render(&painter, QRectF(0, 0, width, height));
    painter.end();
    return pixmap;
}

}  // namespace f2c_cpp
