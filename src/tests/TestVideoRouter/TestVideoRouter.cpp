// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "VideoRouter.h"

#include <QObject>
#include <QtTest>

#include <cstdint>
#include <set>
#include <vector>

namespace {

/// Stands in for the server's ACL lookups. Permissions are mutable so the tests can revoke them
/// mid-stream, which is the case that matters: Mumble's ACLs are editable at runtime and users move
/// between channels, so an authorisation is only ever true "as of now".
struct FakePermissions {
	std::set< std::uint32_t > mayShare;
	std::set< std::pair< std::uint32_t, std::uint32_t > > mayWatch;

	bool canSend(std::uint32_t sender) const { return mayShare.count(sender) > 0; }

	bool canReceive(std::uint32_t subscriber, std::uint32_t sender) const {
		return mayWatch.count({ subscriber, sender }) > 0;
	}
};

} // namespace

class TestVideoRouter : public QObject {
	Q_OBJECT
private slots:
	void sharingRequiresPermission();
	void subscribingRequiresPermission();
	void cannotSubscribeToAStreamThatDoesNotExist();
	void cannotSubscribeToYourself();
	void deliveryRechecksPermissionEveryTime();
	void revokingTheSendersRightStopsTheStream();
	void revalidateReportsWhatItDropped();
	void endingAStreamDropsItsSubscribers();
	void removingAUserCleansUpBothRoles();
	void streamsPerSenderAreCapped();
	void subscriptionsPerUserAreCapped();
	void unsubscribingIsIdempotent();
};

#define ROUTER(perms)                                                                                 \
	VideoRouter router([&perms](std::uint32_t s, std::uint32_t d) { return perms.canReceive(s, d); }, \
					   [&perms](std::uint32_t s) { return perms.canSend(s); })

void TestVideoRouter::sharingRequiresPermission() {
	FakePermissions perms;
	ROUTER(perms);

	// A user with no ShareVideo permission cannot open a stream at all.
	QVERIFY(!router.announceStream(1, 0, true));
	QVERIFY(!router.hasStream(1, 0));

	perms.mayShare.insert(1);

	QVERIFY(router.announceStream(1, 0, true));
	QVERIFY(router.hasStream(1, 0));
}

void TestVideoRouter::subscribingRequiresPermission() {
	FakePermissions perms;
	perms.mayShare.insert(1);

	ROUTER(perms);
	QVERIFY(router.announceStream(1, 0, true));

	// The attack this exists to stop: every client knows every session id from the user list, including
	// for channels it cannot enter. Knowing the id must not be enough.
	QVERIFY(!router.subscribe(2, 1, 0, true));
	QVERIFY(router.subscribersOf(1, 0).empty());

	perms.mayWatch.insert({ 2, 1 });

	QVERIFY(router.subscribe(2, 1, 0, true));
	QCOMPARE(router.subscribersOf(1, 0), std::vector< std::uint32_t >{ 2 });
}

void TestVideoRouter::cannotSubscribeToAStreamThatDoesNotExist() {
	FakePermissions perms;
	perms.mayWatch.insert({ 2, 1 });

	ROUTER(perms);

	// Refused rather than remembered. A pre-subscription would mean a client automatically receives a
	// stream the moment someone starts sharing, without a permission check at that later moment.
	QVERIFY(!router.subscribe(2, 1, 0, true));
	QCOMPARE(router.subscriptionCount(), static_cast< std::size_t >(0));
}

void TestVideoRouter::cannotSubscribeToYourself() {
	FakePermissions perms;
	perms.mayShare.insert(1);
	perms.mayWatch.insert({ 1, 1 });

	ROUTER(perms);
	QVERIFY(router.announceStream(1, 0, true));

	QVERIFY(!router.subscribe(1, 1, 0, true));
}

void TestVideoRouter::deliveryRechecksPermissionEveryTime() {
	FakePermissions perms;
	perms.mayShare.insert(1);
	perms.mayWatch.insert({ 2, 1 });

	ROUTER(perms);
	QVERIFY(router.announceStream(1, 0, true));
	QVERIFY(router.subscribe(2, 1, 0, true));

	QCOMPARE(router.subscribersOf(1, 0).size(), static_cast< std::size_t >(1));

	// The subscriber loses access -- moved channel, or an operator changed the ACL. The next packet must
	// not reach them, without anyone having to remember to tidy the subscription up first.
	perms.mayWatch.clear();

	QVERIFY(router.subscribersOf(1, 0).empty());

	// And it comes back if access is restored, since nothing was destroyed.
	perms.mayWatch.insert({ 2, 1 });
	QCOMPARE(router.subscribersOf(1, 0).size(), static_cast< std::size_t >(1));
}

void TestVideoRouter::revokingTheSendersRightStopsTheStream() {
	FakePermissions perms;
	perms.mayShare.insert(1);
	perms.mayWatch.insert({ 2, 1 });
	perms.mayWatch.insert({ 3, 1 });

	ROUTER(perms);
	QVERIFY(router.announceStream(1, 0, true));
	QVERIFY(router.subscribe(2, 1, 0, true));
	QVERIFY(router.subscribe(3, 1, 0, true));

	QCOMPARE(router.subscribersOf(1, 0).size(), static_cast< std::size_t >(2));

	// Taking ShareVideo away must stop the stream immediately, not merely prevent a new one.
	perms.mayShare.clear();

	QVERIFY(router.subscribersOf(1, 0).empty());
}

void TestVideoRouter::revalidateReportsWhatItDropped() {
	FakePermissions perms;
	perms.mayShare.insert(1);
	perms.mayWatch.insert({ 2, 1 });
	perms.mayWatch.insert({ 3, 1 });

	ROUTER(perms);
	QVERIFY(router.announceStream(1, 0, true));
	QVERIFY(router.subscribe(2, 1, 0, true));
	QVERIFY(router.subscribe(3, 1, 0, true));

	perms.mayWatch.erase({ 3, 1 });

	const std::vector< VideoRouter::DroppedSubscription > dropped = router.revalidate();

	// Reported rather than silently discarded, so the client can be told its subscription ended instead
	// of being left wondering why the picture froze.
	QCOMPARE(dropped.size(), static_cast< std::size_t >(1));
	QCOMPARE(dropped[0].subscriber, 3u);
	QCOMPARE(dropped[0].sender, 1u);

	QCOMPARE(router.subscriptionCount(), static_cast< std::size_t >(1));
	QCOMPARE(router.subscribersOf(1, 0), std::vector< std::uint32_t >{ 2 });

	// A second pass has nothing left to do.
	QVERIFY(router.revalidate().empty());
}

void TestVideoRouter::endingAStreamDropsItsSubscribers() {
	FakePermissions perms;
	perms.mayShare.insert(1);
	perms.mayWatch.insert({ 2, 1 });

	ROUTER(perms);
	QVERIFY(router.announceStream(1, 0, true));
	QVERIFY(router.subscribe(2, 1, 0, true));

	QVERIFY(router.announceStream(1, 0, false));

	QVERIFY(!router.hasStream(1, 0));
	QCOMPARE(router.subscriptionCount(), static_cast< std::size_t >(0));

	// Ending an unknown stream is tolerated rather than treated as an error.
	QVERIFY(router.announceStream(1, 99, false));
}

void TestVideoRouter::removingAUserCleansUpBothRoles() {
	FakePermissions perms;
	perms.mayShare.insert(1);
	perms.mayShare.insert(2);
	perms.mayWatch.insert({ 2, 1 });
	perms.mayWatch.insert({ 1, 2 });

	ROUTER(perms);
	QVERIFY(router.announceStream(1, 0, true));
	QVERIFY(router.announceStream(2, 0, true));
	QVERIFY(router.subscribe(2, 1, 0, true));
	QVERIFY(router.subscribe(1, 2, 0, true));

	QCOMPARE(router.streamCount(), static_cast< std::size_t >(2));
	QCOMPARE(router.subscriptionCount(), static_cast< std::size_t >(2));

	// User 2 disconnects. Their own stream goes, and so does their subscription to user 1.
	router.removeUser(2);

	QCOMPARE(router.streamCount(), static_cast< std::size_t >(1));
	QCOMPARE(router.subscriptionCount(), static_cast< std::size_t >(0));
	QVERIFY(router.hasStream(1, 0));
	QVERIFY(router.subscribersOf(1, 0).empty());
}

void TestVideoRouter::streamsPerSenderAreCapped() {
	FakePermissions perms;
	perms.mayShare.insert(1);

	ROUTER(perms);

	for (std::uint32_t i = 0; i < VideoRouter::MAX_STREAMS_PER_SENDER; ++i) {
		QVERIFY(router.announceStream(1, i, true));
	}

	// Announcing streams is cheap for a client and costs the server memory, so it is bounded.
	QVERIFY(!router.announceStream(1, 999, true));
	QCOMPARE(router.streamCount(), VideoRouter::MAX_STREAMS_PER_SENDER);

	// Re-announcing an existing stream is an update, not a new stream, so it stays allowed.
	QVERIFY(router.announceStream(1, 0, true));
}

void TestVideoRouter::subscriptionsPerUserAreCapped() {
	FakePermissions perms;

	ROUTER(perms);

	for (std::uint32_t sender = 100; sender < 100 + VideoRouter::MAX_SUBSCRIPTIONS_PER_USER + 5; ++sender) {
		perms.mayShare.insert(sender);
		perms.mayWatch.insert({ 1, sender });
		QVERIFY(router.announceStream(sender, 0, true));
	}

	std::size_t accepted = 0;

	for (std::uint32_t sender = 100; sender < 100 + VideoRouter::MAX_SUBSCRIPTIONS_PER_USER + 5; ++sender) {
		if (router.subscribe(1, sender, 0, true)) {
			accepted++;
		}
	}

	QCOMPARE(accepted, VideoRouter::MAX_SUBSCRIPTIONS_PER_USER);
}

void TestVideoRouter::unsubscribingIsIdempotent() {
	FakePermissions perms;
	perms.mayShare.insert(1);
	perms.mayWatch.insert({ 2, 1 });

	ROUTER(perms);
	QVERIFY(router.announceStream(1, 0, true));
	QVERIFY(router.subscribe(2, 1, 0, true));

	QVERIFY(router.subscribe(2, 1, 0, false));
	QVERIFY(router.subscribe(2, 1, 0, false));

	QCOMPARE(router.subscriptionCount(), static_cast< std::size_t >(0));

	// Unsubscribing does not need permission: a user who has just lost access must still be able to
	// tidy up, and refusing would strand the subscription.
	perms.mayWatch.clear();
	QVERIFY(router.subscribe(2, 1, 0, false));
}

QTEST_MAIN(TestVideoRouter)
#include "TestVideoRouter.moc"
