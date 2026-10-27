// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_PLAYOUTDELAY_H_
#define MUMBLE_MUMBLE_PLAYOUTDELAY_H_

#include <algorithm>
#include <cstdint>
#include <limits>

/**
 * How long the receiver should hold a frame before showing it.
 *
 * ## What this replaces
 *
 * A frame used to be shown at `capture + minimumTransit + 200 ms` flat. The 200 was sized for the
 * worst thing the buffer has to survive - a lost fragment being asked for and re-sent, which costs a
 * request delay plus a round trip - and it was paid on every frame of every stream whether anything
 * had been lost or not. On a clean local network that is most of the delay between one person's screen
 * changing and another person seeing it, and none of it is buying anything.
 *
 * ## Why this needs no network measurements
 *
 * The thing worth noticing is that a retransmission is already visible in the arrival times. A frame
 * whose fragment had to be re-sent *arrives late*, and how late a frame arrives is exactly
 * `arrival - capture`. So the buffer does not have to know the round trip, ask the ping machinery for
 * it, or detect loss and switch modes: it only has to be as deep as frames are actually late.
 *
 *     delay = clamp(MINIMUM_MSEC, lateness(PERCENTILE) + MARGIN_MSEC, MAXIMUM_MSEC)
 *
 * where lateness is measured against the fastest frame seen recently. MAXIMUM_MSEC is the old
 * constant, so the worst case is no worse than it was before; a path that really does need 200 ms
 * still gets 200 ms, and only the paths that never needed it stop paying.
 *
 * ## Why the clocks here are not comparable
 *
 * A capture timestamp is microseconds since that capture source started. It is not a wall clock and
 * has no defined relationship to the receiver's clock, so `arrival - capture` is a transit delay plus
 * an arbitrary constant offset, and only *differences* between transit delays mean anything. That is
 * why everything here is relative to the smallest transit seen, and why the smallest transit is
 * tracked over a moving window rather than for all time: two clocks drift, and a minimum held forever
 * would eventually be from a clock relationship that no longer exists.
 *
 * ## Rise fast, fall slow
 *
 * Needing more buffer is urgent - too little and frames are shown before the frames they come after.
 * Needing less is not urgent at all. So an increase applies at once and a decrease is rationed to
 * FALL_MSEC_PER_SECOND, because re-timing a picture under the person watching it is exactly the
 * stutter the buffer exists to prevent.
 *
 * Deliberately header-only and free of Qt: it is arithmetic over two integers, every test that needs
 * it gets it without a build-system change, and `VideoGrid` is compiled standalone by its own test
 * suite.
 */
class PlayoutDelayEstimator {
public:
	/// The old fixed delay, now the ceiling: no path waits longer than it used to.
	static constexpr std::int64_t MAXIMUM_MSEC = 200;
	/// A floor of roughly one frame. Below this there is no buffer at all, so two frames that arrive in
	/// the same millisecond are shown in the same millisecond and one of them is never seen.
	static constexpr std::int64_t MINIMUM_MSEC = 20;
	/// Added to the measured lateness, for the frame that is a little later than any seen so far.
	static constexpr std::int64_t MARGIN_MSEC = 10;
	/// How much of the distribution of lateness to cover. Not the maximum: one 180 ms outlier should not
	/// hold the buffer at 190 ms for ten seconds, because showing that one frame late costs one frame
	/// and holding every frame costs every frame.
	static constexpr int PERCENTILE = 95;
	/// How fast the delay may shrink. 50 ms a second means frames are shown 5 % sooner than they were
	/// captured while it is shrinking, which is not visible in a picture, and that the full 200 ms down
	/// to the floor takes under four seconds rather than happening in one visible step.
	static constexpr std::int64_t FALL_MSEC_PER_SECOND = 50;
	/// Frames that must have been timed before the measurement is believed - about two seconds at thirty
	/// frames a second.
	///
	/// Without this the buffer collapses to its floor on the first frame, because the 95th percentile of
	/// one sample is that sample, and the opening seconds of a stream are the least representative part
	/// of it: the encoder is settling, the first keyframe is enormous, and a second sharer may be about to
	/// start. Starting deep and shrinking is invisible. Starting shallow and discovering that the path
	/// needed more is a stutter, at the exact moment the person is looking to see whether this works.
	static constexpr int MINIMUM_SAMPLES = 60;

	/// Width of one lateness bucket, and how many there are: 5 ms resolution up to the ceiling, which is
	/// finer than anybody can see and coarse enough that the histogram is a cache line or two.
	static constexpr std::int64_t BUCKET_MSEC = 5;
	static constexpr int BUCKETS              = static_cast< int >(MAXIMUM_MSEC / BUCKET_MSEC) + 1;

	/// How long each window of samples covers. Matches what the minimum-transit tracking used before.
	static constexpr std::int64_t WINDOW_MSEC = 10000;

	/**
	 * Records one frame's arrival.
	 *
	 * @param captureMsec The sender's capture timestamp, in milliseconds on the sender's own clock.
	 * @param arrivalMsec When it arrived, in milliseconds on this machine's clock.
	 */
	void noteArrival(std::int64_t captureMsec, std::int64_t arrivalMsec) {
		if (m_windowStartMsec == 0 || arrivalMsec - m_windowStartMsec >= WINDOW_MSEC) {
			m_previousMinMsec = m_windowMinMsec;
			m_previous        = m_current;
			m_windowMinMsec   = std::numeric_limits< std::int64_t >::max();
			m_current         = Histogram();
			m_windowStartMsec = arrivalMsec;
		}

		const std::int64_t transit = arrivalMsec - captureMsec;
		const std::int64_t wasMin  = minTransitMsec();

		m_windowMinMsec = std::min(m_windowMinMsec, transit);

		// Lateness is measured against the smallest transit, so a smaller one makes every lateness
		// already recorded an underestimate. There is no cheap way to shift a histogram, and no reason to
		// want one: the minimum dropping means the two clocks' relationship has changed, and lateness
		// measured against the old relationship is not a measurement of anything. Start again.
		if (wasMin != std::numeric_limits< std::int64_t >::max() && minTransitMsec() < wasMin - BUCKET_MSEC) {
			m_current         = Histogram();
			m_previous        = Histogram();
			m_previousMinMsec = std::numeric_limits< std::int64_t >::max();
		}

		m_current.add(transit - minTransitMsec());
	}

	/// Whether any frame has been seen, and so whether minTransitMsec() means anything.
	bool hasEstimate() const { return minTransitMsec() != std::numeric_limits< std::int64_t >::max(); }

	/// The smallest transit delay in the current or previous window: the transit with no queueing in it.
	std::int64_t minTransitMsec() const { return std::min(m_windowMinMsec, m_previousMinMsec); }

	/**
	 * How long to hold a frame, with the ramp applied.
	 *
	 * @param nowMsec This machine's clock, which is what rations how fast the delay may fall.
	 */
	std::int64_t delayMsec(std::int64_t nowMsec) {
		const std::int64_t target = targetMsec();

		if (m_delayMsec < 0 || target >= m_delayMsec) {
			// The first frame, or more buffer needed: take it now.
			m_delayMsec   = target;
			m_rampedMsec = nowMsec;
			return m_delayMsec;
		}

		const std::int64_t elapsed = nowMsec - m_rampedMsec;

		if (elapsed > 0) {
			const std::int64_t allowed = (elapsed * FALL_MSEC_PER_SECOND) / 1000;

			if (allowed > 0) {
				m_delayMsec  = std::max(target, m_delayMsec - allowed);
				m_rampedMsec = nowMsec;
			}
		}

		return m_delayMsec;
	}

	/// What the delay would be with no ramp: the measurement, clamped. Exposed for tests and for the
	/// statistics line, where seeing the target and the actual apart explains a picture that is settling.
	std::int64_t targetMsec() const {
		const std::int64_t lateness = latenessMsec();

		if (lateness < 0) {
			// Too few frames timed to believe a percentile. See MINIMUM_SAMPLES.
			return MAXIMUM_MSEC;
		}

		return std::clamp(lateness + MARGIN_MSEC, MINIMUM_MSEC, MAXIMUM_MSEC);
	}

private:
	struct Histogram {
		int counts[BUCKETS] = {};
		int total           = 0;

		void add(std::int64_t latenessMsec) {
			const std::int64_t clamped = std::clamp(latenessMsec, std::int64_t(0), MAXIMUM_MSEC);
			const int bucket           = static_cast< int >(clamped / BUCKET_MSEC);

			++counts[bucket];
			++total;
		}
	};

	/// The PERCENTILE-th percentile of lateness across both windows, or -1 before MINIMUM_SAMPLES frames
	/// have been timed.
	///
	/// The upper edge of the chosen bucket is returned rather than its middle, because the question being
	/// asked is "how late can a frame be and still be shown in time", and answering it with the middle of
	/// a bucket is wrong by half a bucket in the direction that drops frames.
	std::int64_t latenessMsec() const {
		const int total = m_current.total + m_previous.total;

		if (total < MINIMUM_SAMPLES) {
			return -1;
		}

		// Integer arithmetic, rounding up, so that the threshold for a handful of samples is the last one
		// rather than none of them.
		const int wanted = (total * PERCENTILE + 99) / 100;
		int seen         = 0;

		for (int bucket = 0; bucket < BUCKETS; ++bucket) {
			seen += m_current.counts[bucket] + m_previous.counts[bucket];

			if (seen >= wanted) {
				return (bucket + 1) * BUCKET_MSEC;
			}
		}

		return MAXIMUM_MSEC;
	}

	std::int64_t m_windowStartMsec = 0;
	std::int64_t m_windowMinMsec   = std::numeric_limits< std::int64_t >::max();
	std::int64_t m_previousMinMsec = std::numeric_limits< std::int64_t >::max();
	Histogram m_current;
	Histogram m_previous;

	/// The delay in force, and when it was last allowed to fall. Negative until the first frame.
	std::int64_t m_delayMsec  = -1;
	std::int64_t m_rampedMsec = 0;
};

#endif // MUMBLE_MUMBLE_PLAYOUTDELAY_H_
