/** @file
 * @author Edouard DUPIN 
 * @copyright 2011, Edouard DUPIN, all right reserved
 * @license APACHE v2.0 (see license file)
 */

#if !defined(__AUDIO_ORCHESTRA_API_ANDROID_NATIVE_H__) && defined(ORCHESTRA_BUILD_JAVA)
#define __AUDIO_ORCHESTRA_API_ANDROID_NATIVE_H__

#include <audio/orchestra/DeviceInfo.h>
#include <audio/orchestra/mode.h>
#include <audio/orchestra/error.h>
#include <audio/orchestra/StreamOptions.h>
#include <audio/format.h>

namespace audio {
	namespace orchestra {
		namespace api {
			namespace android {
				uint32_t getDeviceCount();
				audio::orchestra::DeviceInfo getDeviceInfo(uint32_t _device);
				int32_t open(uint32_t _device,
				             audio::orchestra::mode _mode,
				             uint32_t _channels,
				             uint32_t _firstChannel,
				             uint32_t _sampleRate,
				             audio::format _format,
				             uint32_t *_bufferSize,
				             const audio::orchestra::StreamOptions& _options);
				enum audio::orchestra::error closeStream(int32_t _id);
				enum audio::orchestra::error startStream(int32_t _id);
				enum audio::orchestra::error stopStream(int32_t _id);
				enum audio::orchestra::error abortStream(int32_t _id);
			}
		}
	}
}

#endif
