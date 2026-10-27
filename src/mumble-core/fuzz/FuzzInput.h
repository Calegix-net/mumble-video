// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_FUZZINPUT_H_
#define MUMBLE_FUZZINPUT_H_

#include <cstddef>
#include <cstdint>
#include <span>

namespace MumbleFuzz {

/**
 * Reads a fuzzer's one flat buffer as the sequence of choices a target actually needs.
 *
 * A reassembler is a state machine: single packets find almost nothing, and the interesting bugs live
 * in what the fourth packet does to the state the first three built. So each target consumes its input
 * as a script - take a number, take a chunk, take a number - and this is the cursor that does it.
 *
 * Running off the end is not an error and does not throw. It yields zeros and empty chunks, which ends
 * the target's loop naturally. A fuzzer spends most of its time on truncated inputs, and a harness that
 * treats truncation as a finding reports nothing but its own impatience.
 */
class FuzzInput {
public:
	FuzzInput(const std::uint8_t *data, std::size_t size) : m_data(data), m_size(size) {}

	/// Bytes not yet consumed.
	std::size_t remaining() const { return m_size - m_offset; }

	/// One byte, or zero at the end of the input.
	std::uint8_t byte() {
		if (m_offset >= m_size) {
			return 0;
		}

		return m_data[m_offset++];
	}

	/// A byte folded into [0, count), for picking one of a handful of behaviours. Zero if count is zero.
	std::uint8_t choice(std::uint8_t count) { return count ? static_cast< std::uint8_t >(byte() % count) : 0; }

	/// Big-endian 16 bits, or zero at the end of the input.
	std::uint16_t word() { return static_cast< std::uint16_t >((static_cast< std::uint16_t >(byte()) << 8) | byte()); }

	/**
	 * The next length-prefixed chunk: a big-endian 16-bit length, then that many bytes.
	 *
	 * The length is clamped to what is left rather than rejected, so that truncating any input by one
	 * byte still exercises the same path with a shorter final packet - which is exactly the shape a
	 * mutator produces most often.
	 */
	std::span< const std::uint8_t > chunk() {
		std::size_t length = word();

		if (length > remaining()) {
			length = remaining();
		}

		const std::uint8_t *start = m_data + m_offset;
		m_offset += length;

		return { start, length };
	}

private:
	const std::uint8_t *m_data;
	std::size_t m_size;
	std::size_t m_offset = 0;
};

} // namespace MumbleFuzz

#endif // MUMBLE_FUZZINPUT_H_
