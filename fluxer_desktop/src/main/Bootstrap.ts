// SPDX-License-Identifier: AGPL-3.0-or-later

import {createRequire} from 'node:module';

const requireModule = createRequire(import.meta.url);

if (process.platform === 'win32') {
	// Shortcut repair (via the native @fluxer/win-shell addon) used to run here
	// unconditionally, fire-and-forget, on every launch of an installed
	// (non-portable) build. It is the confirmed source of a recurring
	// STATUS_HEAP_CORRUPTION crash on installed builds of this fork (portable
	// builds never call this path, since it no-ops without a Velopack
	// "current"/Update.exe layout, and never crash). It only repairs shortcut
	// icons/AUMIDs after installs/updates, so disabling it is a cosmetic
	// tradeoff, not a functional one.
	const {VelopackApp} = requireModule('velopack') as typeof import('velopack');
	VelopackApp.build().run();
}

await import(new URL('./MainApp.js', import.meta.url).href);
