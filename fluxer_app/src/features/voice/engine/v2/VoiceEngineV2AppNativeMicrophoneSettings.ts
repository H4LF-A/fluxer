// SPDX-License-Identifier: AGPL-3.0-or-later

import assert from 'node:assert/strict';
import VoiceDevicePermissionState from '@app/features/voice/engine/VoiceDevicePermissionState';
import type VoiceSettingsStore from '@app/features/voice/state/VoiceSettings';
import {normaliseAudioBitrateBps} from '@app/features/voice/utils/AudioPublishOptions';
import {resolveEffectiveDeviceId} from '@app/features/voice/utils/VoiceDeviceManager';
import {
	getActiveInputDeviceLabel,
	resolveVoiceProcessingFromStateForDeviceLabel,
	type VoiceProcessingMode,
} from '@app/features/voice/utils/VoiceProcessingProfile';
import type {VoiceEngineV2MicrophoneOptions} from '@fluxer/voice_engine_v2';

export interface VoiceEngineV2NativeMicrophonePublishOptions extends VoiceEngineV2MicrophoneOptions {
	maxBitrateBps?: number;
}

// Mirrors the floors the vendored livekit-client's PCTransport applies to the
// browser/JS publish path's Opus SDP (opusMaxAverageBitrateBps /
// opusVoiceMaxAverageBitrateBps). The native publish path has no SDP munging
// step, so without an explicit maxBitrateBps here it fell through to
// libwebrtc's own default sender bitrate whenever a channel had no custom
// bitrate override - audibly worse than the browser on identical channels.
const NATIVE_MICROPHONE_STUDIO_MAX_BITRATE_BPS = 510000;
const NATIVE_MICROPHONE_VOICE_MAX_BITRATE_BPS = 64000;

export function resolveVoiceEngineV2NativeMicrophoneMaxBitrateBps(
	channelBitrateBps: number | null | undefined,
	mode: VoiceProcessingMode = 'voice',
): number | undefined {
	assert.ok(
		channelBitrateBps == null || typeof channelBitrateBps === 'number',
		'native microphone channel bitrate must be a number when provided',
	);
	const defaultMaxBitrateBps =
		mode === 'studio' ? NATIVE_MICROPHONE_STUDIO_MAX_BITRATE_BPS : NATIVE_MICROPHONE_VOICE_MAX_BITRATE_BPS;
	const maxBitrateBps = normaliseAudioBitrateBps(channelBitrateBps) ?? defaultMaxBitrateBps;
	assert.ok(maxBitrateBps > 0, 'native microphone max bitrate must be positive when resolved');
	return maxBitrateBps;
}

export function resolveVoiceEngineV2NativeMicrophoneDeviceId(store: typeof VoiceSettingsStore): string {
	assert.equal(typeof store.getInputDeviceId, 'function', 'native microphone settings store requires getInputDeviceId');
	const storedDeviceId = store.getInputDeviceId();
	assert.equal(typeof storedDeviceId, 'string', 'native microphone stored input device id must be a string');
	const {inputDevices} = VoiceDevicePermissionState.getState();
	if (inputDevices.length === 0) return storedDeviceId;
	const effectiveDeviceId = resolveEffectiveDeviceId(storedDeviceId, inputDevices);
	assert.ok(effectiveDeviceId !== null, 'native microphone device id must resolve when input devices are present');
	return effectiveDeviceId;
}

export function resolveVoiceEngineV2NativeMicrophonePublishOptions(
	store: typeof VoiceSettingsStore,
	options: VoiceEngineV2NativeMicrophonePublishOptions = {},
	channelBitrateBps: number | null = null,
): VoiceEngineV2NativeMicrophonePublishOptions {
	assert.ok(store !== null && typeof store === 'object', 'native microphone settings store must be an object');
	assert.equal(typeof store.getInputDeviceId, 'function', 'native microphone settings store requires getInputDeviceId');
	assert.ok(options !== null && typeof options === 'object', 'native microphone options must be an object');
	const activeInputDeviceLabel = getActiveInputDeviceLabel(store);
	const profile = resolveVoiceProcessingFromStateForDeviceLabel(store, activeInputDeviceLabel);
	const maxBitrateBps =
		options.maxBitrateBps ?? resolveVoiceEngineV2NativeMicrophoneMaxBitrateBps(channelBitrateBps, profile.mode);
	return {
		deviceId: options.deviceId ?? resolveVoiceEngineV2NativeMicrophoneDeviceId(store),
		echoCancellation: options.echoCancellation ?? profile.echoCancellation,
		noiseSuppression: options.noiseSuppression ?? profile.browserNoiseSuppression,
		autoGainControl: options.autoGainControl ?? profile.autoGainControl,
		deepFilter: options.deepFilter ?? profile.deepFilter,
		deepFilterNoiseReductionLevel: options.deepFilterNoiseReductionLevel ?? profile.deepFilterNoiseReductionLevel,
		...(maxBitrateBps !== undefined ? {maxBitrateBps} : {}),
	};
}
