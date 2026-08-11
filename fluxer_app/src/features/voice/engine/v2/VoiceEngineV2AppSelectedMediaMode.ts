// SPDX-License-Identifier: AGPL-3.0-or-later

import assert from 'node:assert/strict';
import {isNativeVoiceEngineSelected} from '@app/features/voice/engine/native_voice_engine/NativeVoiceEngineSelection';
import type {VoiceEngineV2AppSelectedMediaMode} from '@app/features/voice/engine/v2/VoiceEngineV2AppSelectedMediaExecutionAdapter';
import {guessPlatform, isDesktop} from '@app/features/ui/utils/NativeUtils';

export interface VoiceEngineV2AppSelectedMediaFlows<T> {
	readonly js: () => Promise<T>;
	readonly native: () => Promise<T>;
}

export function resolveVoiceEngineV2AppSelectedMediaMode(): VoiceEngineV2AppSelectedMediaMode {
	const mode = isNativeVoiceEngineSelected() ? 'native' : 'js';
	assert.ok(mode === 'native' || mode === 'js', 'selected media mode must be native or js');
	return mode;
}

// Windows' native capture pipeline (win-game-capture -> D3D11 texture -> NVENC bridge) has no
// working Windows GPU-texture encode path, so display screen share is routed through Chromium's
// own getDisplayMedia instead - this also gets Chromium's own cursor compositing for free,
// sidestepping a documented Windows Graphics Capture cursor-invisibility bug in fullscreen games.
export function resolveVoiceEngineV2AppSelectedDisplayScreenShareMediaMode(): VoiceEngineV2AppSelectedMediaMode {
	if (isDesktop() && guessPlatform() === 'windows') return 'js';
	return resolveVoiceEngineV2AppSelectedMediaMode();
}

export async function routeVoiceEngineV2AppSelectedMedia<T>(
	flows: VoiceEngineV2AppSelectedMediaFlows<T>,
	modeOverride?: VoiceEngineV2AppSelectedMediaMode,
): Promise<T> {
	assert.equal(typeof flows.js, 'function', 'selected media routing requires a js flow');
	assert.equal(typeof flows.native, 'function', 'selected media routing requires a native flow');
	const mode = modeOverride ?? resolveVoiceEngineV2AppSelectedMediaMode();
	assert.ok(mode === 'native' || mode === 'js', 'selected media mode must be native or js');
	if (mode === 'native') {
		return flows.native();
	}
	return flows.js();
}
