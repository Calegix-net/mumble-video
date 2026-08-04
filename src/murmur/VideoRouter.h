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

protected:
	struct StreamKey {
		std::uint32_t sender;
		std::uint32_t streamID;

		bool operator<(const StreamKey &other) const;
	};

	ReceiveAuthorizer m_mayReceive;
	SendAuthorizer m_maySend;

	// Subscribers per live stream. A stream present here with an empty set is live but unwatched, which
	// is a normal and useful state: the sender should stop encoding.
	std::map< StreamKey, std::set< std::uint32_t > > m_streams;

	// Reverse index, so that removing a user does not require scanning every stream.
	std::map< std::uint32_t, std::set< StreamKey > > m_subscriptions;
};

#endif // MUMBLE_MURMUR_VIDEOROUTER_H_
