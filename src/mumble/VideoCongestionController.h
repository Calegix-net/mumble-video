// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_VIDEOCONGESTIONCONTROLLER_H_
#define MUMBLE_MUMBLE_VIDEOCONGESTIONCONTROLLER_H_

#include <cstdint>
#include <map>

/**
 * Sender-side video congestion control for one outgoing stream.
 *
 * A viewer reports the fraction of this stream's coded units it failed to reassemble
 * (VideoSubscribe.observed_loss_permille, relayed to us by the server). This controller folds those
 * reports - one encoder serves many viewers - into a single loss figure and runs additive-increase /
 * multiplicative-decrease on the encoder's target bitrate: back off multiplicatively when loss rises,
 * creep back up a step at a time when it is clear. The point is to stop a sender blindly transmitting
 * more than a congested link can carry, which only produces loss, NACK storms and frozen pictures.
 *
 * It is deliberately Qt-free and drives nothing itself: feed it reports and NACKs, call tick() once a
 * second, and apply the bitrate it returns. That keeps the control law unit-testable without a network.
 *
 * Absent feedback (every viewer on an old client, or an old server that does not relay it) it holds the
 * configured bitrate exactly - identical to a build without congestion control. The environment variable
 * MUMBLE_VIDEO_CC=0 pins it there unconditionally, the A/B control for measuring the feature.
 */
class VideoCongestionController {
public:
	/// @param screenShare picks the per-kind floor (a screen share must stay legible; a camera may drop
	///   lower). Also lengthens the probe cooldown, since a screen keyframe is a large burst.
	explicit VideoCongestionController(bool screenShare);

	/// The configured target bitrate (kbit/s), already clamped to the server ceiling and the user's cap by
	/// the caller. This is the absolute maximum the controller will ever ask for; it never raises the rate
	/// above what the sender would have used with no congestion control. Call on start and whenever it
	/// changes. The first call also seeds the current rate to the ceiling.
	void setCeiling(unsigned int kbps);

	/// A receiver's feedback report, as relayed by the server.
	/// @param reporterSession the viewer (0 if the server predates reporter_session: all such fold into one
	///   bucket, degrading aggregation but staying correct).
	void onFeedback(std::uint32_t reporterSession, unsigned int lossPermille, std::uint64_t lastCompleteFrame,
					std::int64_t nowMs);

	/// A retransmission request this stream received, and whether a resend-budget hit occurred this
	/// interval. NACKs only ever justify a *decrease* (they saturate under rate caps, so they under-count
	/// heavy loss and must never be read as "healthy").
	void onNack(std::uint32_t requesterSession, std::int64_t nowMs);
	void onResendBudgetHit(std::int64_t /* nowMs */) { m_resendBudgetHit = true; }

	/// Advance the controller. Returns the target bitrate (kbit/s) to apply now; equal to the previous
	/// value when nothing should change (apply unconditionally - it is cheap, and equal means no-op).
	unsigned int tick(std::int64_t nowMs);

	unsigned int currentBitrate() const { return m_current; }
	unsigned int floor() const { return m_floor; }
	bool enabled() const { return m_enabled; }

	// Control-law constants (parts-per-thousand loss unless noted). Exposed for the unit test.
	static constexpr unsigned int LOSS_PROBE    = 5;    // below this: consider probing up
	static constexpr unsigned int LOSS_DECREASE = 25;   // at/above: multiplicative decrease
	static constexpr unsigned int LOSS_SEVERE   = 100;  // at/above: harder decrease
	static constexpr double DECREASE_FACTOR        = 0.85;
	static constexpr double DECREASE_FACTOR_SEVERE = 0.75;
	static constexpr double EWMA_ALPHA             = 0.4;
	static constexpr std::int64_t COOLDOWN_DECR_MS  = 2000;
	static constexpr std::int64_t COOLDOWN_PROBE_MS = 4000;
	// A viewer is considered gone only after this long without a report. Must be well above the feedback
	// interval AND above how late a report can arrive: on a congested link the feedback rides the same
	// path as the video, so reports that are sent every second can arrive many seconds apart - exactly
	// when the controller most needs to keep acting on them. Too small and a reporter is dropped and
	// re-admitted (with a fresh grace) between its own reports, so its loss is perpetually in grace and
	// never drives the rate down.
	static constexpr std::int64_t STALE_MS = 12000;
	// A newly-admitted viewer's first reports include its keyframe-join loss, which is not congestion, so
	// they are ignored this long. Only applied on genuine first admission, not on a re-report.
	static constexpr std::int64_t GRACE_MS = 3000;
	static constexpr unsigned int NACK_DISTINCT_REQUESTERS = 3;  // >= this many -> force a decrease

protected:
	struct Reporter {
		// Two views of this viewer's loss, following AIMD's asymmetry: react fast to congestion, recover
		// slowly. lossLast is the most recent report, used to decide a decrease (back off the instant loss
		// appears); lossEwma is smoothed, used only to gate probing up (raise the rate only once loss has
		// been low for a while, not on one lucky clear report).
		double lossLast            = 0.0;
		double lossEwma            = 0.0;
		std::uint64_t lastComplete = 0;
		std::uint64_t prevComplete = 0;   // lastComplete as of the previous tick, for stall detection
		std::int64_t lastSeenMs    = 0;
		std::int64_t graceUntilMs  = 0;
		bool seenThisTick          = false;
	};

	bool m_enabled      = true;   // MUMBLE_VIDEO_CC != "0"
	bool m_screen       = false;
	unsigned int m_ceiling = 0;   // configured/clamped cap, kbit/s
	unsigned int m_floor   = 0;
	unsigned int m_current = 0;

	std::map< std::uint32_t, Reporter > m_reporters;

	// Probe-and-retreat memory.
	double m_failedLevel    = 1e18;   // last target that drew loss; do not probe back above 0.9x of it
	std::int64_t m_cooldownUntilMs = 0;
	bool m_probedLastTick   = false;
	unsigned int m_preProbe = 0;

	// Per-interval NACK accumulation, cleared each tick.
	std::map< std::uint32_t, char > m_nackRequestersThisInterval;
	bool m_resendBudgetHit = false;

	unsigned int floorFor(unsigned int ceiling) const;
};

#endif // MUMBLE_MUMBLE_VIDEOCONGESTIONCONTROLLER_H_
