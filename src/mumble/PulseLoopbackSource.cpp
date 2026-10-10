// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "PulseLoopbackSource.h"

#include "PulseAudio.h"

#include <QtCore/QByteArray>
#include <QtCore/QLibrary>

#include <pulse/proplist.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

#include <sys/time.h>
#include <unistd.h>

namespace {

// Not in PulseAudio's own wrapper on purpose: it is only needed here, and making it a hard requirement
// there would take Mumble's ordinary PulseAudio input/output down on a libpulse that lacks it.
using SetMonitorStreamFn = int (*)(pa_stream *, std::uint32_t);

SetMonitorStreamFn resolveSetMonitorStream() {
	static const SetMonitorStreamFn fn = []() -> SetMonitorStreamFn {
		for (const char *name : { "libpulse.so.0", "libpulse.so" }) {
			if (QFunctionPointer resolved = QLibrary::resolve(QLatin1String(name), "pa_stream_set_monitor_stream")) {
				return reinterpret_cast< SetMonitorStreamFn >(resolved);
			}
		}
		return nullptr;
	}();

	return fn;
}

std::uint64_t monotonicUsec() {
	return static_cast< std::uint64_t >(
		std::chrono::duration_cast< std::chrono::microseconds >(std::chrono::steady_clock::now().time_since_epoch())
			.count());
}

// Mumble's own playback streams - see PulseAudio.cpp - plus anything else announcing itself as Mumble.
const char *const OWN_STREAM_NAMES[] = { "Mumble Speakers", "Mumble Speakers (Echo)" };

} // namespace

PulseLoopbackSource::PulseLoopbackSource(QObject *parent) : AudioLoopbackSource(parent) {
}

PulseLoopbackSource::~PulseLoopbackSource() {
	PulseLoopbackSource::stop();
}

bool PulseLoopbackSource::isAvailable() {
	if (!resolveSetMonitorStream()) {
		return false;
	}

	PulseAudio pa;
	return pa.m_ok;
}

QString PulseLoopbackSource::describe() const {
	return tr("System audio");
}

bool PulseLoopbackSource::start() {
	stop();

	if (!resolveSetMonitorStream()) {
		qWarning("PulseLoopbackSource: libpulse has no pa_stream_set_monitor_stream; cannot capture per application");
		return false;
	}

	m_pa = std::make_unique< PulseAudio >();
	if (!m_pa->m_ok) {
		m_pa.reset();
		return false;
	}

	m_mainloop = m_pa->threaded_mainloop_new();
	if (!m_mainloop) {
		m_pa.reset();
		return false;
	}

	pa_proplist *props = m_pa->proplist_new();
	m_pa->proplist_sets(props, PA_PROP_APPLICATION_NAME, "Mumble");
	m_pa->proplist_sets(props, PA_PROP_APPLICATION_ID, "info.mumble.Mumble");
	m_context = m_pa->context_new_with_proplist(m_pa->threaded_mainloop_get_api(m_mainloop),
												"Mumble screen-share audio", props);
	m_pa->proplist_free(props);

	if (!m_context) {
		m_pa->threaded_mainloop_free(m_mainloop);
		m_mainloop = nullptr;
		m_pa.reset();
		return false;
	}

	m_pa->context_set_state_callback(m_context, &PulseLoopbackSource::contextStateCallback, this);

	// nullptr: the same server Mumble's own audio uses (PULSE_SERVER, or the session default).
	if (m_pa->context_connect(m_context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0
		|| m_pa->threaded_mainloop_start(m_mainloop) < 0) {
		qWarning("PulseLoopbackSource: could not connect: %s", m_pa->strerror(m_pa->context_errno(m_context)));
		m_pa->context_unref(m_context);
		m_context = nullptr;
		m_pa->threaded_mainloop_free(m_mainloop);
		m_mainloop = nullptr;
		m_pa.reset();
		return false;
	}

	m_running = true;
	return true;
}

void PulseLoopbackSource::stop() {
	if (!m_mainloop) {
		return;
	}

	m_pa->threaded_mainloop_lock(m_mainloop);

	if (m_mixTimer) {
		m_pa->threaded_mainloop_get_api(m_mainloop)->time_free(m_mixTimer);
		m_mixTimer = nullptr;
	}

	for (auto &entry : m_captures) {
		pa_stream *stream = entry.second.stream;
		m_pa->stream_set_read_callback(stream, nullptr, nullptr);
		m_pa->stream_disconnect(stream);
		m_pa->stream_unref(stream);
	}
	m_captures.clear();
	m_sinkMonitors.clear();
	m_seenSinkInputs.clear();

	if (m_context) {
		m_pa->context_set_state_callback(m_context, nullptr, nullptr);
		m_pa->context_set_subscribe_callback(m_context, nullptr, nullptr);
		m_pa->context_disconnect(m_context);
		m_pa->context_unref(m_context);
		m_context = nullptr;
	}

	m_pa->threaded_mainloop_unlock(m_mainloop);
	m_pa->threaded_mainloop_stop(m_mainloop);
	m_pa->threaded_mainloop_free(m_mainloop);
	m_mainloop = nullptr;
	m_pa.reset();

	m_running = false;
}

void PulseLoopbackSource::fail(const QString &reason) {
	m_running = false;
	// AudioLoopbackSource consumers connect failed() queued, so this is safe from the mainloop thread.
	emit failed(reason);
}

void PulseLoopbackSource::contextStateCallback(pa_context *context, void *userdata) {
	auto *self = static_cast< PulseLoopbackSource * >(userdata);

	switch (self->m_pa->context_get_state(context)) {
		case PA_CONTEXT_READY: {
			self->m_pa->context_set_subscribe_callback(context, &PulseLoopbackSource::subscribeCallback, self);
			pa_operation *op = self->m_pa->context_subscribe(
				context,
				static_cast< pa_subscription_mask_t >(PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SINK_INPUT),
				nullptr, nullptr);
			if (op) {
				self->m_pa->operation_unref(op);
			}

			self->refresh();
			self->scheduleMix();
			break;
		}
		case PA_CONTEXT_FAILED:
		case PA_CONTEXT_TERMINATED:
			self->fail(tr("The audio server connection was lost."));
			break;
		default:
			break;
	}
}

void PulseLoopbackSource::subscribeCallback(pa_context *, pa_subscription_event_type_t type, std::uint32_t index,
											void *userdata) {
	auto *self = static_cast< PulseLoopbackSource * >(userdata);

	const int facility = type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
	const int kind     = type & PA_SUBSCRIPTION_EVENT_TYPE_MASK;

	if (facility == PA_SUBSCRIPTION_EVENT_SINK_INPUT && kind == PA_SUBSCRIPTION_EVENT_REMOVE) {
		self->stopCapture(index);
		return;
	}

	// A stream appeared or moved to another device, or a device changed: re-list and reconcile.
	self->refresh();
}

void PulseLoopbackSource::refresh() {
	if (!m_context) {
		return;
	}

	// Answered in order on this one context: every sink is known before the sink inputs that play to them.
	if (pa_operation *op = m_pa->context_get_sink_info_list(m_context, &PulseLoopbackSource::sinkInfoCallback, this)) {
		m_pa->operation_unref(op);
	}
	if (pa_operation *op =
			m_pa->context_get_sink_input_info_list(m_context, &PulseLoopbackSource::sinkInputInfoCallback, this)) {
		m_pa->operation_unref(op);
	}
}

void PulseLoopbackSource::sinkInfoCallback(pa_context *, const pa_sink_info *info, int eol, void *userdata) {
	if (eol || !info) {
		return;
	}

	auto *self = static_cast< PulseLoopbackSource * >(userdata);
	if (info->monitor_source_name) {
		self->m_sinkMonitors[info->index] = info->monitor_source_name;
	}
}

bool PulseLoopbackSource::isOwnStream(const pa_sink_input_info *info) const {
	if (info->name) {
		for (const char *own : OWN_STREAM_NAMES) {
			if (std::strcmp(info->name, own) == 0) {
				return true;
			}
		}
	}

	if (const char *pid = m_pa->proplist_gets(info->proplist, PA_PROP_APPLICATION_PROCESS_ID)) {
		if (std::strtol(pid, nullptr, 10) == static_cast< long >(getpid())) {
			return true;
		}
	}

	if (const char *app = m_pa->proplist_gets(info->proplist, PA_PROP_APPLICATION_NAME)) {
		if (std::strcmp(app, "Mumble") == 0) {
			return true;
		}
	}

	return false;
}

void PulseLoopbackSource::sinkInputInfoCallback(pa_context *, const pa_sink_input_info *info, int eol, void *userdata) {
	auto *self = static_cast< PulseLoopbackSource * >(userdata);

	if (!eol) {
		if (info && !self->isOwnStream(info)) {
			self->m_seenSinkInputs[info->index] = info->sink;
		}
		return;
	}

	// End of one listing: what it contained is the truth. Collected per listing and cleared here rather than
	// when the listing was requested, so two overlapping refreshes cannot mix their answers.
	std::vector< std::uint32_t > gone;
	for (const auto &capture : self->m_captures) {
		const auto seen = self->m_seenSinkInputs.find(capture.first);
		if (seen == self->m_seenSinkInputs.end() || seen->second != capture.second.sinkIndex) {
			gone.push_back(capture.first);
		}
	}
	for (const std::uint32_t index : gone) {
		self->stopCapture(index);
	}

	for (const auto &seen : self->m_seenSinkInputs) {
		if (self->m_captures.find(seen.first) == self->m_captures.end()) {
			self->startCapture(seen.first, seen.second);
		}
	}

	self->m_seenSinkInputs.clear();
}

void PulseLoopbackSource::startCapture(std::uint32_t sinkInputIndex, std::uint32_t sinkIndex) {
	const auto monitor = m_sinkMonitors.find(sinkIndex);
	if (monitor == m_sinkMonitors.end()) {
		return;
	}

	pa_sample_spec spec;
	spec.format   = PA_SAMPLE_FLOAT32LE;
	spec.rate     = CAPTURE_SAMPLE_RATE;
	spec.channels = CAPTURE_CHANNELS;

	pa_stream *stream = m_pa->stream_new(m_context, "Mumble screen-share capture", &spec, nullptr);
	if (!stream) {
		return;
	}

	if (resolveSetMonitorStream()(stream, sinkInputIndex) < 0) {
		m_pa->stream_unref(stream);
		return;
	}

	m_pa->stream_set_read_callback(stream, &PulseLoopbackSource::streamReadCallback, this);

	// Small fragments: these feed a live share, and the mix timer runs every MIX_INTERVAL_MSEC anyway.
	pa_buffer_attr attr;
	attr.maxlength = static_cast< std::uint32_t >(-1);
	attr.tlength   = static_cast< std::uint32_t >(-1);
	attr.prebuf    = static_cast< std::uint32_t >(-1);
	attr.minreq    = static_cast< std::uint32_t >(-1);
	attr.fragsize  = CAPTURE_SAMPLE_RATE * CAPTURE_CHANNELS * sizeof(float) * MIX_INTERVAL_MSEC / 1000;

	const auto flags = static_cast< pa_stream_flags_t >(PA_STREAM_ADJUST_LATENCY | PA_STREAM_DONT_MOVE);

	if (m_pa->stream_connect_record(stream, monitor->second.c_str(), &attr, flags) < 0) {
		m_pa->stream_set_read_callback(stream, nullptr, nullptr);
		m_pa->stream_unref(stream);
		return;
	}

	Capture capture;
	capture.stream    = stream;
	capture.sinkIndex = sinkIndex;
	m_captures.emplace(sinkInputIndex, std::move(capture));

	qDebug("PulseLoopbackSource: capturing sink input %u via %s", sinkInputIndex, monitor->second.c_str());
}

void PulseLoopbackSource::stopCapture(std::uint32_t sinkInputIndex) {
	const auto it = m_captures.find(sinkInputIndex);
	if (it == m_captures.end()) {
		return;
	}

	m_pa->stream_set_read_callback(it->second.stream, nullptr, nullptr);
	m_pa->stream_disconnect(it->second.stream);
	m_pa->stream_unref(it->second.stream);
	m_captures.erase(it);

	qDebug("PulseLoopbackSource: stopped capturing sink input %u", sinkInputIndex);
}

void PulseLoopbackSource::streamReadCallback(pa_stream *stream, std::size_t, void *userdata) {
	auto *self = static_cast< PulseLoopbackSource * >(userdata);

	Capture *capture = nullptr;
	for (auto &entry : self->m_captures) {
		if (entry.second.stream == stream) {
			capture = &entry.second;
			break;
		}
	}

	const std::size_t maxSamples =
		static_cast< std::size_t >(CAPTURE_SAMPLE_RATE) * CAPTURE_CHANNELS * MAX_BACKLOG_MSEC / 1000;

	for (;;) {
		const void *data  = nullptr;
		std::size_t bytes = 0;

		if (self->m_pa->stream_peek(stream, &data, &bytes) < 0 || bytes == 0) {
			return;
		}

		if (capture) {
			const std::size_t samples = bytes / sizeof(float);
			if (data) {
				const auto *floats = static_cast< const float * >(data);
				capture->samples.insert(capture->samples.end(), floats, floats + samples);
			} else {
				// A hole in the stream: silence of the same length keeps it aligned.
				capture->samples.insert(capture->samples.end(), samples, 0.0f);
			}

			if (capture->samples.size() > maxSamples) {
				capture->samples.erase(capture->samples.begin(),
									   capture->samples.begin()
										   + static_cast< std::ptrdiff_t >(capture->samples.size() - maxSamples));
			}
		}

		self->m_pa->stream_drop(stream);
	}
}

void PulseLoopbackSource::scheduleMix() {
	pa_mainloop_api *api = m_pa->threaded_mainloop_get_api(m_mainloop);

	timeval tv;
	gettimeofday(&tv, nullptr);
	tv.tv_usec += MIX_INTERVAL_MSEC * 1000;
	if (tv.tv_usec >= 1000000) {
		tv.tv_sec += 1;
		tv.tv_usec -= 1000000;
	}

	if (m_mixTimer) {
		api->time_restart(m_mixTimer, &tv);
	} else {
		m_mixTimer = api->time_new(api, &tv, &PulseLoopbackSource::mixTimerCallback, this);
	}
}

void PulseLoopbackSource::mixTimerCallback(pa_mainloop_api *, pa_time_event *, const struct timeval *, void *userdata) {
	auto *self = static_cast< PulseLoopbackSource * >(userdata);
	self->mix();
	self->scheduleMix();
}

void PulseLoopbackSource::mix() {
	const std::uint64_t now = monotonicUsec();

	if (m_captures.empty()) {
		// Nothing is playing: send nothing, and start the clock afresh when something does, so the first
		// mix after a quiet spell is not a burst covering the whole silence.
		m_mixStartUsec = 0;
		return;
	}

	if (m_mixStartUsec == 0) {
		m_mixStartUsec = now;
		m_mixedFrames  = 0;
		return;
	}

	std::uint64_t dueFrames = (now - m_mixStartUsec) * CAPTURE_SAMPLE_RATE / 1000000 - m_mixedFrames;

	// After a long hiccup, emit at most MAX_BACKLOG_MSEC rather than everything the clock says is owed.
	const std::uint64_t maxFrames = static_cast< std::uint64_t >(CAPTURE_SAMPLE_RATE) * MAX_BACKLOG_MSEC / 1000;
	if (dueFrames > maxFrames) {
		m_mixedFrames += dueFrames - maxFrames;
		dueFrames = maxFrames;
	}

	if (dueFrames == 0) {
		return;
	}

	const std::size_t samples = static_cast< std::size_t >(dueFrames) * CAPTURE_CHANNELS;
	QByteArray pcm(static_cast< int >(samples * sizeof(float)), Qt::Uninitialized);
	auto *out = reinterpret_cast< float * >(pcm.data());
	std::fill(out, out + samples, 0.0f);

	for (auto &entry : m_captures) {
		std::deque< float > &queue = entry.second.samples;
		const std::size_t take     = std::min(samples, queue.size());
		for (std::size_t i = 0; i < take; ++i) {
			out[i] += queue[i];
		}
		queue.erase(queue.begin(), queue.begin() + static_cast< std::ptrdiff_t >(take));
	}

	// Several loud applications at once can sum past full scale; clamp rather than let the encoder clip.
	for (std::size_t i = 0; i < samples; ++i) {
		out[i] = std::clamp(out[i], -1.0f, 1.0f);
	}

	m_mixedFrames += dueFrames;

	emit samplesReady(pcm, now);
}
