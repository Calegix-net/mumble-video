// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_AUDIOLOOPBACKSOURCE_H_
#define MUMBLE_MUMBLE_AUDIOLOOPBACKSOURCE_H_

#include <QtCore/QByteArray>
#include <QtCore/QObject>
#include <QtCore/QString>

#include <cstdint>

/**
 * Something that produces PCM audio frames captured from system output (loopback), for a screen share's
 * accompanying audio.
 *
 * Deliberately not AudioInput. AudioInput is a single global instance built around exactly one
 * microphone, one Opus encoder, one voice target and the MumbleUDP.Audio wire message, which carries no
 * stream identifier and is decoded into a single per-sender jitter buffer on the other end - a second
 * concurrent source under the same session would corrupt it. This interface exists so that screen-share
 * audio can instead ride the video transport as a stream of its own (see the OpusAudio codec in
 * Mumble.proto's VideoState), entirely independent of the microphone: a user can share a screen with
 * audio while muted, or while not running AudioInput at all.
 *
 * Modelled on VideoSource - start()/stop()/isRunning()/describe(), a data-ready signal and a failed()
 * signal - for the same reason: platform backends differ, and nothing above this interface should need
 * to know how the platform in question does loopback capture.
 */
class AudioLoopbackSource : public QObject {
	Q_OBJECT

public:
	explicit AudioLoopbackSource(QObject *parent = nullptr) : QObject(parent) {}
	~AudioLoopbackSource() override = default;

	/**
	 * Begins capturing. Returns whether the source started; a missing or busy output device fails here
	 * rather than silently producing nothing.
	 */
	virtual bool start() = 0;

	virtual void stop() = 0;

	virtual bool isRunning() const = 0;

	/// Human-readable description of what is being captured, for the UI and for logs.
	virtual QString describe() const = 0;

	/// Sample rate of the frames samplesReady() delivers. Only meaningful once start() has succeeded -
	/// loopback capture uses whatever the output device's current mix format is, which is not always
	/// knowable in advance.
	virtual unsigned int sampleRate() const = 0;

	/// Channel count of the frames samplesReady() delivers, interleaved if more than one.
	virtual unsigned int channelCount() const = 0;

signals:
	/**
	 * @param pcm Interleaved 32-bit float samples, sampleRate()/channelCount() apply, packed as raw
	 *   bytes. A QByteArray rather than a raw pointer specifically so this signal is safe to deliver
	 *   across threads: a capture backend's own buffer (WASAPI's GetBuffer result, for one) is typically
	 *   only valid until the backend releases it, and a queued cross-thread connection - which every
	 *   consumer of this signal needs, since capture necessarily runs on its own thread - copies a
	 *   QByteArray's data, not merely a pointer to it. A raw pointer would dangle by the time a queued
	 *   receiver got to it.
	 * @param captureTimestampUsec Microseconds on a monotonic clock, taken as close to capture as the
	 *   backend allows. Not comparable across sources or with VideoSource's clock.
	 */
	void samplesReady(const QByteArray &pcm, std::uint64_t captureTimestampUsec);

	/**
	 * Emitted when capture stops for a reason other than a stop() call: the output device was removed,
	 * permission was revoked, or the backend failed.
	 */
	void failed(const QString &reason);
};

#endif // MUMBLE_MUMBLE_AUDIOLOOPBACKSOURCE_H_
