// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

// Screen-share audio buffers belong to AudioOutput, which may delete them at any time: an audio restart
// (Settings, the Audio Wizard), the sender leaving, a wipe. MainWindow used to keep a raw pointer to each
// buffer and fed the next packet into freed memory - a crash, or Opus's channel-count assertion. These
// tests feed packets across every one of those deletions and check a fresh buffer is made each time.
// Run under AddressSanitizer they also show nothing is touched after it was freed.

#include "AudioOutput.h"
#include "AudioOutputBuffer.h"
#include "AudioOutputScreenShare.h"
#include "ClientUser.h"

#include <QObject>
#include <QtCore/QByteArray>
#include <QtCore/QReadLocker>
#include <QtTest>

#include <opus.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace {

constexpr unsigned int MIXER_FREQ = 48000;
constexpr int FRAME_SIZE          = 960;

/// An AudioOutput with no device behind it. The mixer thread is never started; pull() does what it does
/// each period instead, up to the point where the buffers have produced their samples.
class TestOutput : public AudioOutput {
public:
	explicit TestOutput(unsigned int mixerFreq) { iMixerFreq = mixerFreq; }

	void run() override {}

	/// The mixer's side of the locking: qrwlOutputs for read, then every buffer's prepareSampleBuffer().
	void pull(unsigned int frameCount) {
		QReadLocker lock(&qrwlOutputs);

		QList< AudioOutputBuffer * > mix;
		QList< AudioOutputBuffer * > del;
		prepareOutputBuffers(frameCount, mix, del);
	}

	int screenShareCount() {
		QReadLocker lock(&qrwlOutputs);

		int count = 0;
		for (AudioOutputBuffer *buffer : qmOutputs) {
			if (qobject_cast< AudioOutputScreenShare * >(buffer)) {
				++count;
			}
		}

		return count;
	}

	/// What happens to every buffer when the mixer is reset, as the Audio Wizard does.
	void dropEverything() { wipe(); }

	/// One mixer period, and the RMS of what the screen-share buffer contributed to it: what the listener
	/// hears from it, before the mixer's own master volume.
	double pullRms(unsigned int frameCount) {
		QReadLocker lock(&qrwlOutputs);

		QList< AudioOutputBuffer * > mix;
		QList< AudioOutputBuffer * > del;
		prepareOutputBuffers(frameCount, mix, del);

		for (AudioOutputBuffer *buffer : mix) {
			if (qobject_cast< AudioOutputScreenShare * >(buffer)) {
				double sum = 0.0;
				for (unsigned int i = 0; i < frameCount * 2; ++i) {
					sum += static_cast< double >(buffer->pfBuffer[i]) * buffer->pfBuffer[i];
				}

				// The mixer multiplies by this too; unity for every buffer today, but part of the level.
				return std::sqrt(sum / (frameCount * 2)) * buffer->volumeMultiplier();
			}
		}

		return -1.0;
	}
};

/// Real Opus packets, as ScreenAudioBroadcaster sends them: 48 kHz stereo, 20 ms each.
std::vector< QByteArray > encodePackets(int count) {
	int error            = 0;
	OpusEncoder *encoder = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &error);
	if (error != OPUS_OK) {
		return {};
	}

	std::vector< QByteArray > packets;
	std::vector< float > pcm(static_cast< std::size_t >(FRAME_SIZE) * 2);
	unsigned char data[4000];

	for (int i = 0; i < count; ++i) {
		for (int frame = 0; frame < FRAME_SIZE; ++frame) {
			const float sample = 0.3f * std::sin(static_cast< float >(i * FRAME_SIZE + frame) * 0.06f);
			pcm[static_cast< std::size_t >(frame) * 2]     = sample;
			pcm[static_cast< std::size_t >(frame) * 2 + 1] = sample;
		}

		const opus_int32 bytes =
			opus_encode_float(encoder, pcm.data(), FRAME_SIZE, data, static_cast< opus_int32 >(sizeof(data)));
		if (bytes > 0) {
			packets.emplace_back(reinterpret_cast< const char * >(data), bytes);
		}
	}

	opus_encoder_destroy(encoder);

	return packets;
}

} // namespace

class TestAudioOutputScreenShare : public QObject {
	Q_OBJECT

private:
	std::vector< QByteArray > m_packets;

	const QByteArray &packet(std::uint64_t sequence) const { return m_packets[sequence % m_packets.size()]; }

private slots:
	void initTestCase() {
		m_packets = encodePackets(50);
		QCOMPARE(m_packets.size(), std::size_t(50));
	}

	void createsOnFirstPacketThenFeedsIt() {
		TestOutput output(MIXER_FREQ);
		ClientUser sender;

		QVERIFY(output.addScreenShareUnit(&sender, 7, 0, packet(0), 1.0f));
		QVERIFY(!output.addScreenShareUnit(&sender, 7, 1, packet(1), 1.0f));
		QCOMPARE(output.screenShareCount(), 1);
	}

	void keepsStreamsOfOneSenderApart() {
		TestOutput output(MIXER_FREQ);
		ClientUser sender;

		QVERIFY(output.addScreenShareUnit(&sender, 7, 0, packet(0), 1.0f));
		QVERIFY(output.addScreenShareUnit(&sender, 8, 0, packet(0), 1.0f));
		QCOMPARE(output.screenShareCount(), 2);

		output.removeScreenShare(&sender, 7);
		QCOMPARE(output.screenShareCount(), 1);

		QVERIFY(!output.addScreenShareUnit(&sender, 8, 1, packet(1), 1.0f));
		QVERIFY(output.addScreenShareUnit(&sender, 7, 1, packet(1), 1.0f));

		output.removeScreenShares(&sender);
		QCOMPARE(output.screenShareCount(), 0);
	}

	void dropsPacketsBeforeTheMixerStarts() {
		TestOutput output(0);
		ClientUser sender;

		QVERIFY(!output.addScreenShareUnit(&sender, 7, 0, packet(0), 1.0f));
		QCOMPARE(output.screenShareCount(), 0);
	}

	void startsAgainAfterRemoveUser() {
		TestOutput output(MIXER_FREQ);
		ClientUser sender;

		QVERIFY(output.addScreenShareUnit(&sender, 7, 0, packet(0), 1.0f));
		output.removeUser(&sender);
		QCOMPARE(output.screenShareCount(), 0);

		QVERIFY(output.addScreenShareUnit(&sender, 7, 1, packet(1), 1.0f));
		QCOMPARE(output.screenShareCount(), 1);
	}

	void startsAgainAfterWipe() {
		TestOutput output(MIXER_FREQ);
		ClientUser sender;

		QVERIFY(output.addScreenShareUnit(&sender, 7, 0, packet(0), 1.0f));
		output.dropEverything();

		QVERIFY(output.addScreenShareUnit(&sender, 7, 1, packet(1), 1.0f));
		QCOMPARE(output.screenShareCount(), 1);
	}

	void startsAgainInANewOutput() {
		// What opening Settings does: Audio::stop() destroys the output with its buffers, Audio::start()
		// makes a new one.
		ClientUser sender;

		auto output = std::make_unique< TestOutput >(MIXER_FREQ);
		QVERIFY(output->addScreenShareUnit(&sender, 7, 0, packet(0), 1.0f));
		QVERIFY(!output->addScreenShareUnit(&sender, 7, 1, packet(1), 1.0f));

		output.reset();
		output = std::make_unique< TestOutput >(MIXER_FREQ);

		QVERIFY(output->addScreenShareUnit(&sender, 7, 2, packet(2), 1.0f));
		QVERIFY(!output->addScreenShareUnit(&sender, 7, 3, packet(3), 1.0f));
		QCOMPARE(output->screenShareCount(), 1);
	}

	void theVolumeChangesWhatIsPlayed() {
		// The tile's slider reaches a playing buffer two ways: setScreenShareVolume() straight away, and the
		// level carried with every packet (MainWindow passes the slider's level to addScreenShareUnit). Both
		// have to change what the mixer gets, by the factor asked for.
		TestOutput output(MIXER_FREQ);
		ClientUser sender;

		std::uint64_t sequence = 0;
		const auto feed        = [&](int packets, float volume) {
			for (int i = 0; i < packets; ++i, ++sequence) {
				output.addScreenShareUnit(&sender, 7, sequence, packet(sequence), volume);
			}
		};

		feed(20, 1.0f);

		// Past the prebuffer and Opus's start-up, into the steady tone.
		for (int i = 0; i < 4; ++i) {
			QVERIFY(output.pullRms(480) >= 0.0);
		}

		const double unity = output.pullRms(480);
		QVERIFY2(unity > 0.1, qPrintable(QString::number(unity)));

		output.setScreenShareVolume(&sender, 0.25f);
		const double quarter = output.pullRms(480);
		QVERIFY2(std::abs(quarter / unity - 0.25) < 0.03, qPrintable(QString::number(quarter / unity)));

		output.setScreenShareVolume(&sender, 0.0f);
		QCOMPARE(output.pullRms(480), 0.0);

		// The next packet carries the slider's level; 0.3 at 180% stays under the limiter's knee, so linear.
		feed(1, 1.8f);
		const double boosted = output.pullRms(480);
		QVERIFY2(std::abs(boosted / unity - 1.8) < 0.1, qPrintable(QString::number(boosted / unity)));
	}

	void feedsWhileTheMixerPullsAndBuffersGoAway() {
		// The mixer thread pulls the whole time, while this thread feeds and every so often removes the
		// buffer through each path there is. A lock-order mistake would hang here; a buffer used after it
		// was freed is what AddressSanitizer reports.
		TestOutput output(MIXER_FREQ);
		ClientUser sender;

		std::atomic< bool > done{ false };
		std::thread mixer([&]() {
			while (!done.load()) {
				output.pull(480);
			}
		});

		int created  = 0;
		int removals = 0;
		for (std::uint64_t sequence = 0; sequence < 3000; ++sequence) {
			if (output.addScreenShareUnit(&sender, 7, sequence, packet(sequence), 1.5f)) {
				++created;
			}

			output.setScreenShareVolume(&sender, 1.2f);

			switch (sequence % 400) {
				case 100:
					output.removeUser(&sender);
					++removals;
					break;
				case 200:
					output.removeScreenShare(&sender, 7);
					++removals;
					break;
				case 300:
					output.dropEverything();
					++removals;
					break;
				case 399:
					output.removeScreenShares(&sender);
					++removals;
					break;
				default:
					break;
			}
		}

		done.store(true);
		mixer.join();

		// The first packet made one, and so did the packet after every removal.
		QCOMPARE(created, 1 + removals);
	}
};

QTEST_GUILESS_MAIN(TestAudioOutputScreenShare)
#include "TestAudioOutputScreenShare.moc"
