// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MURMUR_VIDEOROUTER_H_
#define MUMBLE_MURMUR_VIDEOROUTER_H_

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <vector>

/**
 * Decides who receives whose video.
 *
 * Video is subscribe-on-demand rather than fanned out to the whole channel like audio. Audio has
 * natural silence and costs a few kilobits; a screen share costs megabits and does not stop, so sending
 * it to everyone present regardless of whether they are looking at it would be the difference between a
 * server that works and one that does not.
 *
 * The security property this class exists to enforce is that **a subscription is an authorisation
 * decision, not a lookup**. Every client learns every other client's session id from the user list,
 * including for channels it cannot enter, so a router that honoured a subscription because the session
 * id was valid would relay a screen share to people with no access to the channel it is shared in.
 * Permission is therefore re-checked on delivery, not only when the subscription is created: Mumble's
 * ACLs are editable at runtime and users move between channels, so an authorisation that was correct a
 * minute ago proves nothing now.
 *
 * Authorisation is injected rather than called directly so that this class can be tested without a
 * running server, and so that the routing logic and the permission model can be reasoned about
 * separately.
 */
class VideoRouter {
public:
	/// Whether `subscriber` is currently allowed to receive video from `sender`.
	using ReceiveAuthorizer = std::function< bool(std::uint32_t subscriber, std::uint32_t sender) >;

	/// Whether `sender` is currently allowed to share video at all.
	using SendAuthorizer = std::function< bool(std::uint32_t sender) >;

	/// Ceiling on how many streams one user may have open, so that announcing streams is not itself a
	/// way to consume server memory.
	static constexpr std::size_t MAX_STREAMS_PER_SENDER = 8;

	/// Ceiling on how many streams one user may subscribe to. A client showing a video grid needs one
	/// per visible participant; well beyond that is a client bug or an attempt to amplify traffic.
	static constexpr std::size_t MAX_SUBSCRIPTIONS_PER_USER = 32;

	VideoRouter(ReceiveAuthorizer mayReceive, SendAuthorizer maySend);

	/**
	 * Registers, updates or ends a stream.
	 *
	 * @param active False ends the stream and drops every subscription to it.
	 * @returns Whether the announcement was accepted. It is refused if the sender lacks permission to
	 *   share video, or already has too many streams open.
	 */
	bool announceStream(std::uint32_t sender, std::uint32_t streamID, bool active);

	/**
	 * Adds or removes a subscription.
	 *
	 * @returns Whether the request was accepted. Refused if the stream does not exist, if the subscriber
	 *   is not permitted to receive from that sender, or if the subscriber is already at its limit.
	 *   Subscribing to a stream that does not exist is refused rather than remembered, so that a client
	 *   cannot pre-subscribe to a user who has not started sharing and receive it automatically later.
	 */
	bool subscribe(std::uint32_t subscriber, std::uint32_t sender, std::uint32_t streamID, bool subscribing);

	/**
	 * The sessions that should receive a packet belonging to this stream, right now.
	 *
	 * Permission is re-checked here for every recipient. That is deliberately the expensive-looking
	 * choice: it is what makes revoking access, or a user moving to another channel, take effect on the
	 * next packet instead of whenever someone remembers to call revalidate().
	 */
	std::vector< std::uint32_t > subscribersOf(std::uint32_t sender, std::uint32_t streamID) const;

	/**
	 * Drops subscriptions that are no longer permitted, and streams whose sender may no longer share.
	 *
	 * subscribersOf() already filters, so this is not needed for correctness. It exists so that state
	 * does not accumulate after an ACL change, and so that a client can be told its subscription ended
	 * rather than silently receiving nothing.
	 *
	 * @returns The (subscriber, sender, stream) triples that were dropped, so the caller can notify.
	 */
	struct DroppedSubscription {
		std::uint32_t subscriber;
		std::uint32_t sender;
		std::uint32_t streamID;
	};
	std::vector< DroppedSubscription > revalidate();

	/// Forgets everything about a user, both as a sender and as a subscriber.
	void removeUser(std::uint32_t session);

	bool hasStream(std::uint32_t sender, std::uint32_t streamID) const;
	std::size_t streamCount() const;
	std::size_t subscriptionCount() const;

	struct StreamKey {
		std::uint32_t sender;
		std::uint32_t streamID;

		bool operator<(const StreamKey &other) const;
	};

	/**
	 * Marks a stream as alive right now, for staleStreams() below to judge future silence against. Meant
	 * to be called for two different reasons, both of which amount to "this is a fresh, known-good
	 * baseline, not evidence anything is wrong yet":
	 *
	 *  - After actually relaying a unit for the stream to at least one recipient - the real liveness
	 *    signal this exists to track.
	 *  - Right after a successful subscribe() adds a subscriber to a stream, particularly its first -
	 *    the moment a fresh grace period is deserved, before anything has had a chance to actually flow
	 *    to that subscriber yet. Announcing a stream with no subscriber does not need this call at all:
	 *    staleStreams() below ignores any stream with none, so there is nothing yet for a clock to judge.
	 *
	 * Deliberately not called automatically from inside subscribe() itself: doing so would mean giving an
	 * already-tested, security-sensitive method with subscription and permission logic of its own a
	 * reason to also depend on a clock, for a concern that is entirely separate from what it already
	 * does. The owner already calls it at the right moment; calling this alongside costs nothing extra to
	 * get right.
	 *
	 * Not meaningful, and not expected to be called, for a stream with no subscribers: there is nothing to
	 * relay to, and that is a normal, indefinitely-lasting state (see m_streams's own comment) that must
	 * never look stale for it. Does nothing if the stream does not exist.
	 *
	 * @param nowMsec Caller-supplied rather than read from a clock in here, so this class stays testable
	 *   without a real one - same reasoning revalidate() and subscribersOf() already follow by taking
	 *   their inputs as plain arguments instead of reaching for global state.
	 */
	void noteRelayed(std::uint32_t sender, std::uint32_t streamID, std::int64_t nowMsec);

	/**
	 * Streams that currently have at least one subscriber but have not actually relayed anything in more
	 * than @p timeoutMsec - almost certainly a sender whose encoder or capture hung while its connection
	 * otherwise stayed healthy. Nothing else here, or in a connection-level timeout elsewhere in the
	 * server, can detect this specific failure: the connection itself never goes quiet, only this one
	 * stream does, and a client watching it has no way to tell "nothing changed" apart from "nothing is
	 * coming" on its own.
	 *
	 * Freshly announced streams are exempt for @p timeoutMsec from the moment they are announced, not
	 * from some earlier default, so a sender who has not sent a first unit yet is not immediately treated
	 * as stale.
	 *
	 * Does not itself end anything - the caller decides what "stale" means to do about it, the same
	 * separation of concerns revalidate() already uses for permission changes.
	 */
	std::vector< StreamKey > staleStreams(std::int64_t nowMsec, std::int64_t timeoutMsec) const;

protected:
	ReceiveAuthorizer m_mayReceive;
	SendAuthorizer m_maySend;

	// Subscribers per live stream. A stream present here with an empty set is live but unwatched, which
	// is a normal and useful state: the sender should stop encoding.
	std::map< StreamKey, std::set< std::uint32_t > > m_streams;

	// Reverse index, so that removing a user does not require scanning every stream.
	std::map< std::uint32_t, std::set< StreamKey > > m_subscriptions;

	// Wall-clock time (caller's own clock, via nowMsec - see noteRelayed()) each live stream last actually
	// relayed a unit, seeded to the moment it was announced. A parallel map rather than folding into
	// m_streams's own value type deliberately: this is new, staleness-only bookkeeping layered on top of
	// the existing, security-sensitive subscription logic above, not something worth touching every
	// existing call site over.
	std::map< StreamKey, std::int64_t > m_lastRelayedMsec;
};

#endif // MUMBLE_MURMUR_VIDEOROUTER_H_
