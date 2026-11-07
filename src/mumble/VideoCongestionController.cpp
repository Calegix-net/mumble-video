// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "VideoCongestionController.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

VideoCongestionController::VideoCongestionController(bool screenShare) : m_screen(screenShare) {
	const char *kill = std::getenv("MUMBLE_VIDEO_CC");
	m_enabled        = !(kill && std::strcmp(kill, "0") == 0);
}

unsigned int VideoCongestionController::floorFor(unsigned int ceiling) const {
	// A screen share has to stay legible, so it floors higher than a camera; both also floor at a fraction
	// of the ceiling so a very low configured bitrate is not dragged below usefulness.
	const unsigned int kindFloor = m_screen ? 400u : 150u;
	const unsigned int frac      = static_cast< unsigned int >(0.15 * ceiling);
	unsigned int floor           = std::max(kindFloor, frac);
	return std::min(floor, ceiling);   // never above the ceiling (a tiny configured cap)
}

void VideoCongestionController::setCeiling(unsigned int kbps) {
	const bool first = (m_ceiling == 0);
	m_ceiling        = kbps;
	m_floor          = floorFor(kbps);

	if (first || !m_enabled) {
		// Start at (and, when disabled, stay pinned to) exactly the configured rate: identical to a build
		// with no congestion control until feedback moves it.
		m_current = kbps;
	} else {
		m_current = std::clamp(m_current, m_floor, m_ceiling);
	}
}

void VideoCongestionController::onFeedback(std::uint32_t reporterSession, unsigned int lossPermille,
										   std::uint64_t lastCompleteFrame, std::int64_t nowMs) {
	auto [it, inserted] = m_reporters.try_emplace(reporterSession);
	Reporter &r         = it->second;

	if (inserted) {
		// A brand-new viewer: its first reports include the keyframe-join loss, which is not congestion.
		r.graceUntilMs = nowMs + GRACE_MS;
		r.lossEwma     = lossPermille;
		r.prevComplete = lastCompleteFrame;
	} else {
		r.lossEwma = EWMA_ALPHA * lossPermille + (1.0 - EWMA_ALPHA) * r.lossEwma;
	}
	r.lossLast     = lossPermille;
	r.lastComplete = std::max(r.lastComplete, lastCompleteFrame);
	r.lastSeenMs   = nowMs;
	r.seenThisTick = true;
}

void VideoCongestionController::onNack(std::uint32_t requesterSession, std::int64_t) {
	m_nackRequestersThisInterval[requesterSession] = 1;
}

unsigned int VideoCongestionController::tick(std::int64_t nowMs) {
	if (!m_enabled || m_ceiling == 0) {
		return m_current;   // disabled, or not configured yet
	}

	// Drop viewers that have gone quiet.
	for (auto it = m_reporters.begin(); it != m_reporters.end();) {
		it = (nowMs - it->second.lastSeenMs > STALE_MS) ? m_reporters.erase(it) : std::next(it);
	}

	// Build two loss votes per live, out-of-grace reporter: the latest report (fast, for decreases) and the
	// EWMA (slow, the probe-up gate). A reporter whose last_complete_frame has not advanced across two ticks
	// while we are sending is stalled - treat it as fully lost on both, so a frozen viewer reporting 0 loss
	// does not read as healthy.
	struct Vote {
		double last;
		double ewma;
	};
	std::vector< Vote > votes;
	for (auto &entry : m_reporters) {
		Reporter &r = entry.second;
		if (nowMs < r.graceUntilMs) {
			r.prevComplete = r.lastComplete;
			r.seenThisTick = false;
			continue;
		}
		Vote v{ r.lossLast, r.lossEwma };
		if (r.seenThisTick && r.lastComplete == r.prevComplete) {
			v.last = v.ewma = 1000.0;   // stalled
		}
		votes.push_back(v);
		r.prevComplete = r.lastComplete;
		r.seenThisTick = false;
	}

	// Aggregate. With four or more live reporters, drop the single worst (by latest loss) as an outlier so
	// one bad link does not starve everyone; with fewer, the worst governs (a small call cannot afford to
	// ignore anyone). aggLast drives decreases; aggEwma gates probing up.
	double aggLast = 0.0;
	double aggEwma = 0.0;
	if (!votes.empty()) {
		std::sort(votes.begin(), votes.end(), [](const Vote &a, const Vote &b) { return a.last < b.last; });
		const std::size_t n = votes.size() >= 4 ? votes.size() - 1 : votes.size();   // drop worst if >=4
		for (std::size_t i = 0; i < n; ++i) {
			aggLast = std::max(aggLast, votes[i].last);
			aggEwma = std::max(aggEwma, votes[i].ewma);
		}
	}

	// NACK override: a decrease only. NACKs saturate under the rate caps, so they under-count heavy loss
	// and may only ever force the rate down, never justify holding or raising it.
	const bool nackPressure =
		m_resendBudgetHit || m_nackRequestersThisInterval.size() >= NACK_DISTINCT_REQUESTERS;
	if (nackPressure) {
		aggLast = std::max(aggLast, static_cast< double >(LOSS_DECREASE));
	}
	m_nackRequestersThisInterval.clear();
	m_resendBudgetHit = false;

	const bool haveSignal = !votes.empty() || nackPressure;

	double target       = m_current;
	bool probedThisTick = false;
	const double curD   = m_current;

	if (!haveSignal) {
		// No live feedback (old peers / nobody watching): hold the configured rate exactly.
		m_probedLastTick = false;
		return m_current;
	}

	// Decreases are rate-limited by the cooldown, not applied every tick: loss persists in aggLast between
	// the (possibly several-seconds-apart) reports, and decreasing on every tick would crash the rate to
	// the floor on a single congestion episode. One multiplicative step per cooldown is the AIMD
	// discipline (a reduction per round trip), and holds the rate steady between steps.
	const bool mayAdjust = nowMs >= m_cooldownUntilMs;

	if (aggLast >= LOSS_SEVERE && mayAdjust) {
		target            = curD * DECREASE_FACTOR_SEVERE;
		m_cooldownUntilMs = nowMs + COOLDOWN_DECR_MS;
		// Remember the level loss appeared at, so the probe does not climb straight back to it - but only
		// while there was room above the floor. Loss that persists once we are pinned at the floor must not
		// collapse the memory to the floor, or the probe gate (cur < 0.9*failedLevel) could never fire and
		// the rate would never recover after the link cleared.
		if (curD > m_floor) {
			m_failedLevel = curD;
		}
	} else if (aggLast >= LOSS_DECREASE && mayAdjust) {
		target            = curD * DECREASE_FACTOR;
		m_cooldownUntilMs = nowMs + COOLDOWN_DECR_MS;
		if (curD > m_floor) {
			m_failedLevel = curD;
		}
	} else if (m_probedLastTick && aggLast >= LOSS_PROBE) {
		// We probed up last tick and loss has re-appeared (but only mildly - a real decrease would have
		// been caught above): give the probe step back and lengthen the cooldown before trying again.
		target            = m_preProbe;
		m_cooldownUntilMs = nowMs + COOLDOWN_PROBE_MS;
	} else if (aggLast < LOSS_PROBE && aggEwma < LOSS_PROBE && nowMs >= m_cooldownUntilMs
			   && curD < 0.90 * m_failedLevel) {
		// Both signals clear and out of cooldown: probe up by an additive step, capped at a tenth of the
		// ceiling.
		const double step = std::min(std::max(50.0, 0.05 * curD), 0.10 * m_ceiling);
		m_preProbe        = m_current;
		target            = curD + step;
		probedThisTick    = true;
	}
	// else: dead-band (LOSS_PROBE..LOSS_DECREASE with no prior probe) or cooldown -> hold.

	target = std::clamp(target, static_cast< double >(m_floor), static_cast< double >(m_ceiling));

	// Anti-churn: ignore a change too small to be worth re-touching the encoder for.
	const double minActuate = std::max(32.0, 0.03 * curD);
	if (std::abs(target - curD) >= minActuate) {
		m_current = static_cast< unsigned int >(target + 0.5);
	}

	m_probedLastTick = probedThisTick;
	return m_current;
}
