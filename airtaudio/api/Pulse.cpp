/**
 * @author Gary P. SCAVONE
 * 
 * @copyright 2001-2013 Gary P. Scavone, all right reserved
 * 
 * @license like MIT (see license file)
 */


#if defined(__LINUX_PULSE__)

#include <unistd.h>
#include <limits.h>
#include <airtaudio/Interface.h>
#include <airtaudio/debug.h>
// Code written by Peter Meerwald, pmeerw@pmeerw.net
// and Tristan Matthews.

#include <pulse/error.h>
#include <pulse/simple.h>
#include <cstdio>

#undef __class__
#define __class__ "api::Pulse"

airtaudio::Api* airtaudio::api::Pulse::Create() {
	return new airtaudio::api::Pulse();
}


static const uint32_t SUPPORTED_SAMPLERATES[] = {
	8000,
	16000,
	22050,
	32000,
	44100,
	48000,
	96000,
	0
};

struct rtaudio_pa_format_mapping_t {
	enum audio::format airtaudio_format;
	pa_sample_format_t pa_format;
};

static const rtaudio_pa_format_mapping_t supported_sampleformats[] = {
	{audio::format_int16, PA_SAMPLE_S16LE},
	{audio::format_int32, PA_SAMPLE_S32LE},
	{audio::format_float, PA_SAMPLE_FLOAT32LE},
	{audio::format_unknow, PA_SAMPLE_INVALID}};

struct PulseAudioHandle {
	pa_simple *s_play;
	pa_simple *s_rec;
	std::thread* thread;
	std::condition_variable runnable_cv;
	bool runnable;
	PulseAudioHandle() :
	  s_play(0),
	  s_rec(0),
	  runnable(false) {
		
	}
};

airtaudio::api::Pulse::~Pulse() {
	if (m_stream.state != airtaudio::state_closed) {
		closeStream();
	}
}

uint32_t airtaudio::api::Pulse::getDeviceCount() {
	return 1;
}

airtaudio::DeviceInfo airtaudio::api::Pulse::getDeviceInfo(uint32_t _device) {
	airtaudio::DeviceInfo info;
	info.probed = true;
	info.name = "PulseAudio";
	info.outputChannels = 2;
	info.inputChannels = 2;
	info.duplexChannels = 2;
	info.isDefaultOutput = true;
	info.isDefaultInput = true;
	for (const uint32_t *sr = SUPPORTED_SAMPLERATES; *sr; ++sr) {
		info.sampleRates.push_back(*sr);
	}
	info.nativeFormats.push_back(audio::format_int16);
	info.nativeFormats.push_back(audio::format_int32);
	info.nativeFormats.push_back(audio::format_float);
	return info;
}

static void pulseaudio_callback(void* _user) {
	airtaudio::CallbackInfo *cbi = static_cast<airtaudio::CallbackInfo *>(_user);
	airtaudio::api::Pulse *context = static_cast<airtaudio::api::Pulse*>(cbi->object);
	volatile bool *isRunning = &cbi->isRunning;
	while (*isRunning) {
		context->callbackEvent();
	}
}

enum airtaudio::error airtaudio::api::Pulse::closeStream() {
	PulseAudioHandle *pah = static_cast<PulseAudioHandle *>(m_stream.apiHandle);
	m_stream.callbackInfo.isRunning = false;
	if (pah) {
		m_stream.mutex.lock();
		if (m_stream.state == airtaudio::state_stopped) {
			pah->runnable = true;
			pah->runnable_cv.notify_one();;
		}
		m_stream.mutex.unlock();
		pah->thread->join();
		if (pah->s_play) {
			pa_simple_flush(pah->s_play, nullptr);
			pa_simple_free(pah->s_play);
		}
		if (pah->s_rec) {
			pa_simple_free(pah->s_rec);
		}
		delete pah;
		m_stream.apiHandle = nullptr;
	}
	if (m_stream.userBuffer[0] != nullptr) {
		free(m_stream.userBuffer[0]);
		m_stream.userBuffer[0] = nullptr;
	}
	if (m_stream.userBuffer[1] != nullptr) {
		free(m_stream.userBuffer[1]);
		m_stream.userBuffer[1] = nullptr;
	}
	m_stream.state = airtaudio::state_closed;
	m_stream.mode = airtaudio::mode_unknow;
	return airtaudio::error_none;
}

void airtaudio::api::Pulse::callbackEvent() {
	PulseAudioHandle *pah = static_cast<PulseAudioHandle *>(m_stream.apiHandle);
	if (m_stream.state == airtaudio::state_stopped) {
		std::unique_lock<std::mutex> lck(m_stream.mutex);
		while (!pah->runnable) {
			pah->runnable_cv.wait(lck);
		}
		if (m_stream.state != airtaudio::state_running) {
			m_stream.mutex.unlock();
			return;
		}
	}
	if (m_stream.state == airtaudio::state_closed) {
		ATA_ERROR("the stream is closed ... this shouldn't happen!");
		return;
	}
	double streamTime = getStreamTime();
	enum airtaudio::status status = airtaudio::status_ok;
	int32_t doStopStream = m_stream.callbackInfo.callback(m_stream.userBuffer[airtaudio::modeToIdTable(airtaudio::mode_output)],
	                                                      m_stream.userBuffer[airtaudio::modeToIdTable(airtaudio::mode_input)],
	                                                      m_stream.bufferSize,
	                                                      streamTime,
	                                                      status);
	if (doStopStream == 2) {
		abortStream();
		return;
	}
	m_stream.mutex.lock();
	void *pulse_in = m_stream.doConvertBuffer[airtaudio::modeToIdTable(airtaudio::mode_input)] ? m_stream.deviceBuffer : m_stream.userBuffer[airtaudio::modeToIdTable(airtaudio::mode_input)];
	void *pulse_out = m_stream.doConvertBuffer[airtaudio::modeToIdTable(airtaudio::mode_output)] ? m_stream.deviceBuffer : m_stream.userBuffer[airtaudio::modeToIdTable(airtaudio::mode_output)];
	if (m_stream.state != airtaudio::state_running) {
		goto unlock;
	}
	int32_t pa_error;
	size_t bytes;
	if (    m_stream.mode == airtaudio::mode_output
	     || m_stream.mode == airtaudio::mode_duplex) {
		if (m_stream.doConvertBuffer[airtaudio::modeToIdTable(airtaudio::mode_output)]) {
			convertBuffer(m_stream.deviceBuffer,
			              m_stream.userBuffer[airtaudio::modeToIdTable(airtaudio::mode_output)],
			              m_stream.convertInfo[airtaudio::modeToIdTable(airtaudio::mode_output)]);
			bytes = m_stream.nDeviceChannels[airtaudio::modeToIdTable(airtaudio::mode_output)] * m_stream.bufferSize * audio::getFormatBytes(m_stream.deviceFormat[airtaudio::modeToIdTable(airtaudio::mode_output)]);
		} else {
			bytes = m_stream.nUserChannels[airtaudio::modeToIdTable(airtaudio::mode_output)] * m_stream.bufferSize * audio::getFormatBytes(m_stream.userFormat);
		}
		if (pa_simple_write(pah->s_play, pulse_out, bytes, &pa_error) < 0) {
			ATA_ERROR("audio write error, " << pa_strerror(pa_error) << ".");
			return;
		}
	}
	if (m_stream.mode == airtaudio::mode_input || m_stream.mode == airtaudio::mode_duplex) {
		if (m_stream.doConvertBuffer[airtaudio::modeToIdTable(airtaudio::mode_input)]) {
			bytes = m_stream.nDeviceChannels[airtaudio::modeToIdTable(airtaudio::mode_input)] * m_stream.bufferSize * audio::getFormatBytes(m_stream.deviceFormat[airtaudio::modeToIdTable(airtaudio::mode_input)]);
		} else {
			bytes = m_stream.nUserChannels[airtaudio::modeToIdTable(airtaudio::mode_input)] * m_stream.bufferSize * audio::getFormatBytes(m_stream.userFormat);
		}
		if (pa_simple_read(pah->s_rec, pulse_in, bytes, &pa_error) < 0) {
			ATA_ERROR("audio read error, " << pa_strerror(pa_error) << ".");
			return;
		}
		if (m_stream.doConvertBuffer[airtaudio::modeToIdTable(airtaudio::mode_input)]) {
			convertBuffer(m_stream.userBuffer[airtaudio::modeToIdTable(airtaudio::mode_input)],
			              m_stream.deviceBuffer,
			              m_stream.convertInfo[airtaudio::modeToIdTable(airtaudio::mode_input)]);
		}
	}
unlock:
	m_stream.mutex.unlock();
	airtaudio::Api::tickStreamTime();
	if (doStopStream == 1) {
		stopStream();
		return;
	}
	return;
}

enum airtaudio::error airtaudio::api::Pulse::startStream() {
	PulseAudioHandle *pah = static_cast<PulseAudioHandle *>(m_stream.apiHandle);
	if (m_stream.state == airtaudio::state_closed) {
		ATA_ERROR("the stream is not open!");
		return airtaudio::error_invalidUse;
	}
	if (m_stream.state == airtaudio::state_running) {
		ATA_ERROR("the stream is already running!");
		return airtaudio::error_warning;
	}
	m_stream.mutex.lock();
	m_stream.state = airtaudio::state_running;
	pah->runnable = true;
	pah->runnable_cv.notify_one();
	m_stream.mutex.unlock();
	return airtaudio::error_none;
}

enum airtaudio::error airtaudio::api::Pulse::stopStream() {
	PulseAudioHandle *pah = static_cast<PulseAudioHandle *>(m_stream.apiHandle);
	if (m_stream.state == airtaudio::state_closed) {
		ATA_ERROR("the stream is not open!");
		return airtaudio::error_invalidUse;
	}
	if (m_stream.state == airtaudio::state_stopped) {
		ATA_ERROR("the stream is already stopped!");
		return airtaudio::error_warning;
	}
	m_stream.state = airtaudio::state_stopped;
	m_stream.mutex.lock();
	if (pah && pah->s_play) {
		int32_t pa_error;
		if (pa_simple_drain(pah->s_play, &pa_error) < 0) {
			ATA_ERROR("error draining output device, " << pa_strerror(pa_error) << ".");
			m_stream.mutex.unlock();
			return airtaudio::error_systemError;
		}
	}
	m_stream.state = airtaudio::state_stopped;
	m_stream.mutex.unlock();
	return airtaudio::error_none;
}

enum airtaudio::error airtaudio::api::Pulse::abortStream() {
	PulseAudioHandle *pah = static_cast<PulseAudioHandle*>(m_stream.apiHandle);
	if (m_stream.state == airtaudio::state_closed) {
		ATA_ERROR("the stream is not open!");
		return airtaudio::error_invalidUse;
	}
	if (m_stream.state == airtaudio::state_stopped) {
		ATA_ERROR("the stream is already stopped!");
		return airtaudio::error_warning;
	}
	m_stream.state = airtaudio::state_stopped;
	m_stream.mutex.lock();
	if (pah && pah->s_play) {
		int32_t pa_error;
		if (pa_simple_flush(pah->s_play, &pa_error) < 0) {
			ATA_ERROR("error flushing output device, " << pa_strerror(pa_error) << ".");
			m_stream.mutex.unlock();
			return airtaudio::error_systemError;
		}
	}
	m_stream.state = airtaudio::state_stopped;
	m_stream.mutex.unlock();
	return airtaudio::error_none;
}

bool airtaudio::api::Pulse::probeDeviceOpen(uint32_t _device,
                                            airtaudio::mode _mode,
                                            uint32_t _channels,
                                            uint32_t _firstChannel,
                                            uint32_t _sampleRate,
                                            audio::format _format,
                                            uint32_t *_bufferSize,
                                            airtaudio::StreamOptions *_options) {
	PulseAudioHandle *pah = 0;
	uint64_t bufferBytes = 0;
	pa_sample_spec ss;
	if (_device != 0) {
		return false;
	}
	if (_mode != airtaudio::mode_input && _mode != airtaudio::mode_output) {
		return false;
	}
	if (_channels != 1 && _channels != 2) {
		ATA_ERROR("unsupported number of channels.");
		return false;
	}
	ss.channels = _channels;
	if (_firstChannel != 0) {
		return false;
	}
	bool sr_found = false;
	for (const uint32_t *sr = SUPPORTED_SAMPLERATES; *sr; ++sr) {
		if (_sampleRate == *sr) {
			sr_found = true;
			m_stream.sampleRate = _sampleRate;
			ss.rate = _sampleRate;
			break;
		}
	}
	if (!sr_found) {
		ATA_ERROR("unsupported sample rate.");
		return false;
	}
	bool sf_found = 0;
	for (const rtaudio_pa_format_mapping_t *sf = supported_sampleformats;
	     sf->airtaudio_format && sf->pa_format != PA_SAMPLE_INVALID;
	     ++sf) {
		if (_format == sf->airtaudio_format) {
			sf_found = true;
			m_stream.userFormat = sf->airtaudio_format;
			ss.format = sf->pa_format;
			break;
		}
	}
	if (!sf_found) {
		ATA_ERROR("unsupported sample format.");
		return false;
	}
	m_stream.deviceInterleaved[modeToIdTable(_mode)] = true;
	m_stream.nBuffers = 1;
	m_stream.doByteSwap[modeToIdTable(_mode)] = false;
	m_stream.doConvertBuffer[modeToIdTable(_mode)] = false;
	m_stream.deviceFormat[modeToIdTable(_mode)] = m_stream.userFormat;
	m_stream.nUserChannels[modeToIdTable(_mode)] = _channels;
	m_stream.nDeviceChannels[modeToIdTable(_mode)] = _channels + _firstChannel;
	m_stream.channelOffset[modeToIdTable(_mode)] = 0;
	// Allocate necessary internal buffers.
	bufferBytes = m_stream.nUserChannels[modeToIdTable(_mode)] * *_bufferSize * audio::getFormatBytes(m_stream.userFormat);
	m_stream.userBuffer[modeToIdTable(_mode)] = (char *) calloc(bufferBytes, 1);
	if (m_stream.userBuffer[modeToIdTable(_mode)] == nullptr) {
		ATA_ERROR("error allocating user buffer memory.");
		goto error;
	}
	m_stream.bufferSize = *_bufferSize;
	if (m_stream.doConvertBuffer[modeToIdTable(_mode)]) {
		bool makeBuffer = true;
		bufferBytes = m_stream.nDeviceChannels[modeToIdTable(_mode)] * audio::getFormatBytes(m_stream.deviceFormat[modeToIdTable(_mode)]);
		if (_mode == airtaudio::mode_input) {
			if (m_stream.mode == airtaudio::mode_output && m_stream.deviceBuffer) {
				uint64_t bytesOut = m_stream.nDeviceChannels[0] * audio::getFormatBytes(m_stream.deviceFormat[0]);
				if (bufferBytes <= bytesOut) makeBuffer = false;
			}
		}
		if (makeBuffer) {
			bufferBytes *= *_bufferSize;
			if (m_stream.deviceBuffer) free(m_stream.deviceBuffer);
			m_stream.deviceBuffer = (char *) calloc(bufferBytes, 1);
			if (m_stream.deviceBuffer == nullptr) {
				ATA_ERROR("error allocating device buffer memory.");
				goto error;
			}
		}
	}
	m_stream.device[modeToIdTable(_mode)] = _device;
	// Setup the buffer conversion information structure.
	if (m_stream.doConvertBuffer[modeToIdTable(_mode)]) {
		setConvertInfo(_mode, _firstChannel);
	}
	if (!m_stream.apiHandle) {
		PulseAudioHandle *pah = new PulseAudioHandle;
		if (!pah) {
			ATA_ERROR("error allocating memory for handle.");
			goto error;
		}
		m_stream.apiHandle = pah;
	}
	pah = static_cast<PulseAudioHandle *>(m_stream.apiHandle);
	int32_t error;
	switch (_mode) {
		case airtaudio::mode_input:
			pah->s_rec = pa_simple_new(nullptr, "airtAudio", PA_STREAM_RECORD, nullptr, "Record", &ss, nullptr, nullptr, &error);
			if (!pah->s_rec) {
				ATA_ERROR("error connecting input to PulseAudio server.");
				goto error;
			}
			break;
		case airtaudio::mode_output:
			pah->s_play = pa_simple_new(nullptr, "airtAudio", PA_STREAM_PLAYBACK, nullptr, "Playback", &ss, nullptr, nullptr, &error);
			if (!pah->s_play) {
				ATA_ERROR("error connecting output to PulseAudio server.");
				goto error;
			}
			break;
		default:
			goto error;
	}
	if (m_stream.mode == airtaudio::mode_unknow) {
		m_stream.mode = _mode;
	} else if (m_stream.mode == _mode) {
		goto error;
	}else {
		m_stream.mode = airtaudio::mode_duplex;
	}
	if (!m_stream.callbackInfo.isRunning) {
		m_stream.callbackInfo.object = this;
		m_stream.callbackInfo.isRunning = true;
		pah->thread = new std::thread(pulseaudio_callback, (void *)&m_stream.callbackInfo);
		if (pah->thread == nullptr) {
			ATA_ERROR("error creating thread.");
			goto error;
		}
	}
	m_stream.state = airtaudio::state_stopped;
	return true;
error:
	if (pah && m_stream.callbackInfo.isRunning) {
		delete pah;
		m_stream.apiHandle = 0;
	}
	for (int32_t i=0; i<2; i++) {
		if (m_stream.userBuffer[i]) {
			free(m_stream.userBuffer[i]);
			m_stream.userBuffer[i] = 0;
		}
	}
	if (m_stream.deviceBuffer) {
		free(m_stream.deviceBuffer);
		m_stream.deviceBuffer = 0;
	}
	return false;
}

#endif
