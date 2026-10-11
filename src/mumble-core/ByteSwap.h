// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_BYTESWAP_H_
#define MUMBLE_BYTESWAP_H_

#include <cstdint>

namespace Mumble {
constexpr std::uint64_t byteSwap64(std::uint64_t x) {
	return ((x & 0x00000000000000FFull) << 56) | ((x & 0x000000000000FF00ull) << 40)
		   | ((x & 0x0000000000FF0000ull) << 24) | ((x & 0x00000000FF000000ull) << 8)
		   | ((x & 0x000000FF00000000ull) >> 8) | ((x & 0x0000FF0000000000ull) >> 24)
		   | ((x & 0x00FF000000000000ull) >> 40) | ((x & 0xFF00000000000000ull) >> 56);
}
} // namespace Mumble

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#	define SWAP64(x) (x)
#else
#	if defined(__GNUC__) || defined(__clang__)
#		define SWAP64(x) __builtin_bswap64(x)
#	else
#		define SWAP64(x) Mumble::byteSwap64(x)
#	endif
#endif

#endif
