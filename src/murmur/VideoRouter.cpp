// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "VideoRouter.h"

#include <tuple>
#include <utility>

bool VideoRouter::StreamKey::operator<(const StreamKey &other) const {
	return std::tie(sender, streamID) < std::tie(other.sender, other.streamID);
}

VideoRouter::VideoRouter(ReceiveAuthorizer mayReceive, SendAuthorizer maySend)
	: m_mayReceive(std::move(mayReceive)), m_maySend(std::move(maySend)) {
}

bool VideoRouter::hasStream(std::uint32_t sender, std::uint32_t streamID) const {
	return m_streams.find(StreamKey{ sender, streamID }) != m_streams.end();
}

std::size_t VideoRouter::streamCount() const {
	return m_streams.size();
}

std::size_t VideoRouter::subscriptionCount() const {
	std::size_t count = 0;

	for (const auto &entry : m_streams) {
		count += entry.second.size();
	}

	return count;
}

bool VideoRouter::announceStream(std::uint32_t sender, std::uint32_t streamID, bool active) {
	const StreamKey key{ sender, streamID };

	if (!active) {
		const auto it = m_streams.find(key);

		if (it == m_streams.end()) {
			// Ending a stream that was never announced is not an error; a client that reconnects and
			// tidies up should not be punished for it.
			return true;
		}

		for (std::uint32_t subscriber : it->second) {
			const auto subIt = m_subscriptions.find(subscriber);

			if (subIt != m_subscriptions.end()) {
				subIt->second.erase(key);

				if (subIt->second.empty()) {
					m_subscriptions.erase(subIt);
				}
			}
		}

		m_streams.erase(it);

		return true;
	}

	if (!m_maySend(sender)) {
		return false;
	}

	if (m_streams.find(key) != m_streams.end()) {
		// Re-announcing an existing stream is how a sender updates its parameters. Subscribers are kept.
		return true;
	}

	// Counted before inserting, so the limit is on what the sender ends up holding.
	std::size_t owned = 0;

	for (const auto &entry : m_streams) {
		if (entry.first.sender == sender) {
			owned++;
		}
	}

	if (owned >= MAX_STREAMS_PER_SENDER) {
		return false;
	}

	m_streams.emplace(key, std::set< std::uint32_t >());

	return true;
}

bool VideoRouter::subscribe(std::uint32_t subscriber, std::uint32_t sender, std::uint32_t streamID, bool subscribing) {
	const StreamKey key{ sender, streamID };

	const auto streamIt = m_streams.find(key);

	if (streamIt == m_streams.end()) {
		return false;
	}

	if (!subscribing) {
		streamIt->second.erase(subscriber);

		const auto subIt = m_subscriptions.find(subscriber);

		if (subIt != m_subscriptions.end()) {
			subIt->second.erase(key);

			if (subIt->second.empty()) {
				m_subscriptions.erase(subIt);
			}
		}

		return true;
	}

	// Subscribing to yourself would make the server echo a client's own camera back to it, wasting
	// exactly the bandwidth this whole mechanism exists to save.
	if (subscriber == sender) {
		return false;
	}

	if (!m_mayReceive(subscriber, sender)) {
		return false;
	}

	if (streamIt->second.find(subscriber) != streamIt->second.end()) {
		return true;
	}

	auto &owned = m_subscriptions[subscriber];

	if (owned.size() >= MAX_SUBSCRIPTIONS_PER_USER) {
		if (owned.empty()) {
			m_subscriptions.erase(subscriber);
		}

		return false;
	}

	owned.insert(key);
	streamIt->second.insert(subscriber);

	return true;
}

std::vector< std::uint32_t > VideoRouter::subscribersOf(std::uint32_t sender, std::uint32_t streamID) const {
	std::vector< std::uint32_t > recipients;

	const auto it = m_streams.find(StreamKey{ sender, streamID });

	if (it == m_streams.end()) {
		return recipients;
	}

	// The sender's own permission is checked too: revoking someone's right to share must stop their
	// stream immediately, not merely prevent them announcing a new one.
	if (!m_maySend(sender)) {
		return recipients;
	}

	recipients.reserve(it->second.size());

	for (std::uint32_t subscriber : it->second) {
		if (m_mayReceive(subscriber, sender)) {
			recipients.push_back(subscriber);
		}
	}

	return recipients;
}

std::vector< VideoRouter::DroppedSubscription > VideoRouter::revalidate() {
	std::vector< DroppedSubscription > dropped;

	for (auto streamIt = m_streams.begin(); streamIt != m_streams.end();) {
		const StreamKey key = streamIt->first;

		if (!m_maySend(key.sender)) {
			for (std::uint32_t subscriber : streamIt->second) {
				dropped.push_back(DroppedSubscription{ subscriber, key.sender, key.streamID });

				const auto subIt = m_subscriptions.find(subscriber);

				if (subIt != m_subscriptions.end()) {
					subIt->second.erase(key);

					if (subIt->second.empty()) {
						m_subscriptions.erase(subIt);
					}
				}
			}

			streamIt = m_streams.erase(streamIt);

			continue;
		}

		for (auto subscriberIt = streamIt->second.begin(); subscriberIt != streamIt->second.end();) {
			if (m_mayReceive(*subscriberIt, key.sender)) {
				++subscriberIt;

				continue;
			}

			dropped.push_back(DroppedSubscription{ *subscriberIt, key.sender, key.streamID });

			const auto subIt = m_subscriptions.find(*subscriberIt);

			if (subIt != m_subscriptions.end()) {
				subIt->second.erase(key);

				if (subIt->second.empty()) {
					m_subscriptions.erase(subIt);
				}
			}

			subscriberIt = streamIt->second.erase(subscriberIt);
		}

		++streamIt;
	}

	return dropped;
}

void VideoRouter::removeUser(std::uint32_t session) {
	// As a subscriber: drop them from every stream they were watching, using the reverse index rather
	// than walking all streams.
	const auto subIt = m_subscriptions.find(session);

	if (subIt != m_subscriptions.end()) {
		for (const StreamKey &key : subIt->second) {
			const auto streamIt = m_streams.find(key);

			if (streamIt != m_streams.end()) {
				streamIt->second.erase(session);
			}
		}

		m_subscriptions.erase(subIt);
	}

	// As a sender: end every stream they own, which also detaches their subscribers.
	for (auto it = m_streams.begin(); it != m_streams.end();) {
		if (it->first.sender != session) {
			++it;

			continue;
		}

		const StreamKey key = it->first;

		for (std::uint32_t subscriber : it->second) {
			const auto otherIt = m_subscriptions.find(subscriber);

			if (otherIt != m_subscriptions.end()) {
				otherIt->second.erase(key);

				if (otherIt->second.empty()) {
					m_subscriptions.erase(otherIt);
				}
			}
		}

		it = m_streams.erase(it);
	}
}
