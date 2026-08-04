// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

// Benchmarks for video fragmentation and reassembly.
//
// These live in the UDP receive path, which on the server is a thread promoted to SCHED_FIFO and shared
// with all audio mixing. Work done here is work not being done for voice, so the numbers that matter are
// the per-packet ones, and in particular how they behave when the reassembler is holding a lot of state.

#include "VideoFragmentation.h"

#include <benchmark/benchmark.h>

#include <string>
#include <vector>

using namespace Mumble::Protocol;

namespace {

std::vector< byte > makePayload(std::size_t size) {
	std::vector< byte > payload(size);

	for (std::size_t i = 0; i < size; ++i) {
		payload[i] = static_cast< byte >(i & 0xFF);
	}

	return payload;
}

VideoUnitHeader makeHeader() {
	VideoUnitHeader header;
	header.senderSession = 1;
	header.streamID      = 1;
	header.width         = 1920;
	header.height        = 1080;

	return header;
}

/// Fills a reassembler with `units` partial units so that the per-packet paths can be measured against a
/// realistically loaded map rather than an empty one.
void preload(VideoReassembler &reassembler, std::size_t units) {
	VideoUnit sink;
	const std::string chunk(64, 'x');

	for (std::size_t i = 0; i < units; ++i) {
		MumbleUDP::Video message;
		message.set_frame_number(i);
		message.set_fragment_count(4);
		message.set_fragment_index(0);
		message.set_payload(chunk);

		const std::size_t size = message.ByteSizeLong();

		std::vector< byte > packet(size + 1);
		packet[0] = static_cast< byte >(UDPMessageType::Video);
		message.SerializeToArray(packet.data() + 1, static_cast< int >(size));

		// Spread across senders so the per-sender cap does not evict them again immediately.
		reassembler.processPacket(packet, static_cast< std::uint32_t >(1 + i / 64), 1, sink);
	}
}

} // namespace

/// Fragmenting a unit of a given size. The steady state matters more than the first call, since the
/// buffers are meant to be reused across frames.
static void BM_fragment(benchmark::State &state) {
	const std::vector< byte > payload = makePayload(static_cast< std::size_t >(state.range(0)));
	const VideoUnitHeader header      = makeHeader();

	VideoFragmenter fragmenter;

	for (auto _ : state) {
		benchmark::DoNotOptimize(fragmenter.fragment(header, payload));
	}

	state.SetBytesProcessed(static_cast< std::int64_t >(state.iterations()) * state.range(0));
}
BENCHMARK(BM_fragment)->Arg(900)->Arg(4000)->Arg(20000);

/// A whole unit through the reassembler, which is what a received tile costs end to end.
static void BM_reassembleUnit(benchmark::State &state) {
	const std::vector< byte > payload = makePayload(static_cast< std::size_t >(state.range(0)));

	VideoFragmenter fragmenter;
	fragmenter.fragment(makeHeader(), payload);

	const std::vector< std::vector< byte > > packets = fragmenter.packets();

	VideoReassembler reassembler;
	VideoUnit unit;
	std::uint64_t frame = 0;

	for (auto _ : state) {
		// A fresh frame number each iteration, so every pass really does build and complete a unit
		// rather than hitting the duplicate path.
		state.PauseTiming();
		std::vector< std::vector< byte > > renumbered = packets;
		for (std::vector< byte > &packet : renumbered) {
			MumbleUDP::Video message;
			message.ParseFromArray(packet.data() + 1, static_cast< int >(packet.size() - 1));
			message.set_frame_number(frame);
			const std::size_t size = message.ByteSizeLong();
			packet.resize(size + 1);
			message.SerializeToArray(packet.data() + 1, static_cast< int >(size));
		}
		frame++;
		state.ResumeTiming();

		for (const std::vector< byte > &packet : renumbered) {
			benchmark::DoNotOptimize(reassembler.processPacket(packet, 1, 1, unit));
		}
	}

	state.SetBytesProcessed(static_cast< std::int64_t >(state.iterations()) * state.range(0));
}
BENCHMARK(BM_reassembleUnit)->Arg(900)->Arg(4000)->Arg(20000);

/// The cost of a garbage packet, as a function of how much state is already being tracked.
///
/// This is the shape that matters for denial of service: if it grows with the number of tracked units,
/// then a peer sending nonsense at line rate can make the real-time thread do work proportional to how
/// busy the server already is.
static void BM_rejectGarbage(benchmark::State &state) {
	VideoReassembler reassembler;
	preload(reassembler, static_cast< std::size_t >(state.range(0)));

	const std::vector< byte > garbage = { static_cast< byte >(UDPMessageType::Video), 0xFF, 0xFF, 0xFF, 0xFF };

	VideoUnit unit;

	for (auto _ : state) {
		benchmark::DoNotOptimize(reassembler.processPacket(garbage, 1, 1, unit));
	}
}
BENCHMARK(BM_rejectGarbage)->Arg(0)->Arg(256)->Arg(2048);

/// The cost of a valid first fragment against a loaded reassembler, which exercises the eviction and
/// expiry bookkeeping rather than just the parse.
static void BM_firstFragmentWhenLoaded(benchmark::State &state) {
	VideoReassembler reassembler;
	preload(reassembler, static_cast< std::size_t >(state.range(0)));

	MumbleUDP::Video message;
	message.set_fragment_count(4);
	message.set_fragment_index(0);
	message.set_payload(std::string(900, 'x'));

	const std::size_t size = message.ByteSizeLong();
	std::vector< byte > packet(size + 1);
	packet[0] = static_cast< byte >(UDPMessageType::Video);
	message.SerializeToArray(packet.data() + 1, static_cast< int >(size));

	VideoUnit unit;
	std::uint32_t session = 5000;

	for (auto _ : state) {
		benchmark::DoNotOptimize(reassembler.processPacket(packet, session++, 1, unit));
	}
}
BENCHMARK(BM_firstFragmentWhenLoaded)->Arg(0)->Arg(256)->Arg(2048);

BENCHMARK_MAIN();
