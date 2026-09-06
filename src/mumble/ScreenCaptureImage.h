// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.
#ifndef MUMBLE_MUMBLE_SCREENCAPTUREIMAGE_H_
#define MUMBLE_MUMBLE_SCREENCAPTUREIMAGE_H_
#include <QImage>
#include <QPoint>
#include <QTransform>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ScreenCaptureImage {
// Values match DXGI_MODE_ROTATION and DXGI_OUTDUPL_POINTER_SHAPE_TYPE. Kept platform
// independent so rotation, clipping, and all three pointer formats can be tested without a GPU.
inline QImage orient(const QImage &image, unsigned int rotation) {
	const int angle = rotation == 2 ? 90 : rotation == 3 ? 180 : rotation == 4 ? 270 : 0;
	return angle ? image.transformed(QTransform().rotate(angle)) : image;
}
inline void drawPointer(QImage &image, QPoint position, unsigned int type, unsigned int width, unsigned int height,
						unsigned int pitch, const unsigned char *data, std::size_t size) {
	if (!data || image.format() != QImage::Format_ARGB32 || !width || !height || !pitch)
		return;
	if (type != 1 && type != 2 && type != 4)
		return;
	const std::uint64_t rowBytes =
		type == 1 ? (static_cast< std::uint64_t >(width) + 7) / 8 : static_cast< std::uint64_t >(width) * 4;
	if (pitch < rowBytes || static_cast< std::uint64_t >(height) * pitch > size || (type == 1 && height % 2))
		return;
	const unsigned int visibleHeight = type == 1 ? height / 2 : height;
	// Iterate only the intersection, including cursors partly outside the display.
	const QRect bounds =
		QRect(position, QSize(static_cast< int >(width), static_cast< int >(visibleHeight))).intersected(image.rect());
	for (int y = bounds.top(); !bounds.isEmpty() && y <= bounds.bottom(); ++y) {
		auto *dest     = reinterpret_cast< QRgb * >(image.scanLine(y));
		const auto row = static_cast< unsigned int >(y - position.y());
		for (int x = bounds.left(); x <= bounds.right(); ++x) {
			const auto column = static_cast< unsigned int >(x - position.x());
			if (type == 1) {
				const unsigned int mask = 0x80u >> (column % 8);
				const bool andBit       = data[static_cast< std::size_t >(row) * pitch + column / 8] & mask;
				const bool xorBit = data[static_cast< std::size_t >(row + visibleHeight) * pitch + column / 8] & mask;
				dest[x] = ((dest[x] & (andBit ? 0x00ffffffu : 0u)) ^ (xorBit ? 0x00ffffffu : 0u)) | 0xff000000u;
			} else {
				QRgb pixel;
				std::memcpy(&pixel, data + static_cast< std::size_t >(row) * pitch + column * 4, sizeof(pixel));
				if (type == 4) {
					dest[x] = (qAlpha(pixel) ? dest[x] ^ (pixel & 0x00ffffffu) : pixel) | 0xff000000u;
				} else {
					const int alpha = qAlpha(pixel);
					// Match the desktop duplication sample's SRC_ALPHA / INV_SRC_ALPHA blend.
					dest[x] = qRgb((qRed(pixel) * alpha + qRed(dest[x]) * (255 - alpha) + 127) / 255,
								   (qGreen(pixel) * alpha + qGreen(dest[x]) * (255 - alpha) + 127) / 255,
								   (qBlue(pixel) * alpha + qBlue(dest[x]) * (255 - alpha) + 127) / 255);
				}
			}
		}
	}
}
} // namespace ScreenCaptureImage
#endif
