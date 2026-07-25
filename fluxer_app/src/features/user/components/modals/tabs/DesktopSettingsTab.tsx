// SPDX-License-Identifier: AGPL-3.0-or-later

import {ConfirmModal} from '@app/features/app/components/dialogs/ConfirmModal';
import {SettingsSection} from '@app/features/app/components/dialogs/shared/SettingsSection';
import {SettingsTabContainer} from '@app/features/app/components/dialogs/shared/SettingsTabLayout';
import {EXAMPLE_DOMAIN, EXAMPLE_URL, PRODUCT_NAME} from '@app/features/app/config/I18nDisplayConstants';
import {normalizeInstanceOrigin} from '@app/features/auth/flow/BrowserLoginHandoffModal';
import {
	getAutostartStatus,
	getCachedAutostartStatus,
	setAutostartEnabled,
} from '@app/features/platform/utils/Autostart';
import {Button} from '@app/features/ui/button/Button';
import * as ModalCommands from '@app/features/ui/commands/ModalCommands';
import {modal} from '@app/features/ui/commands/ModalCommands';
import {Input} from '@app/features/ui/components/form/FormInput';
import {Switch} from '@app/features/ui/components/form/FormSwitch';
import {
	getCachedDesktopWindowBehaviorSettings,
	getDesktopWindowBehaviorPendingRestart,
	getDesktopWindowBehaviorSettings,
	relaunchDesktopApp,
	setDesktopWindowBehaviorSettings,
} from '@app/features/ui/utils/DesktopWindowBehaviorUtils';
import {getElectronAPI, guessPlatform, isDesktop, isNativeLinux, isNativeMacOS} from '@app/features/ui/utils/NativeUtils';
import * as FormUtils from '@app/lib/forms';
import type {DesktopWindowBehaviorSettings} from '@app/types/electron.d';
import {msg} from '@lingui/core/macro';
import {Trans, useLingui} from '@lingui/react/macro';
import {observer} from 'mobx-react-lite';
import type React from 'react';
import {useCallback, useLayoutEffect, useState} from 'react';

const LAUNCH_PRODUCT_AT_LOGIN_DESCRIPTOR = msg({
	message: 'Launch {productName} at login',
	comment: 'Desktop setting label for starting the app automatically after OS sign-in.',
});
const RESTART_PRODUCT_FOR_TRAY_CHANGE_DESCRIPTOR = msg({
	message:
		"Restart {productName} for this change to take effect because Linux desktops can't reliably add or remove tray icons at runtime.",
	comment: 'Desktop setting description shown when a tray-icon change on Linux requires restart.',
});
const RESTART_NOW_DESCRIPTOR = msg({
	message: 'Restart now',
	comment: 'Short label in the desktop settings tab. Keep it concise.',
});
const RESTART_DESCRIPTOR = msg({
	message: 'Restart {productName}?',
	comment: 'Confirmation prompt in the desktop settings tab. Preserve {productName}; it is inserted by code.',
});
const LATER_DESCRIPTOR = msg({
	message: 'Later',
	comment: 'Short label in the desktop settings tab. Keep it concise.',
});
const INSTANCE_URL_DESCRIPTOR = msg({
	message: 'Instance URL',
	comment: 'Desktop setting label for the field showing which server this client is connected to.',
});
const INSTANCE_URL_HINT_DESCRIPTOR = msg({
	message: 'The URL of the {productName} instance to connect to.',
	comment: 'Desktop setting field hint for the instance URL input. Product name is interpolated.',
});
const INVALID_INSTANCE_URL_DESCRIPTOR = msg({
	message: 'Invalid instance URL. Try something like "{exampleDomain}" or "{exampleUrl}".',
	comment: 'Desktop setting validation error for an unparseable instance URL. Example domain and URL are interpolated.',
});
const SWITCH_INSTANCE_DESCRIPTOR = msg({
	message: 'Switch',
	comment: 'Short button label in the desktop settings tab that switches the connected instance.',
});
const SWITCH_INSTANCE_CONFIRM_TITLE_DESCRIPTOR = msg({
	message: 'Switch instance?',
	comment: 'Confirmation prompt title in the desktop settings tab.',
});
const SWITCH_INSTANCE_CONFIRM_DESCRIPTION_DESCRIPTOR = msg({
	message: '{productName} will reload and connect to {instanceUrl}. You will need to sign in again if this is a different account.',
	comment: 'Confirmation prompt body in the desktop settings tab. Product name and target instance URL are interpolated.',
});
const CANCEL_DESCRIPTOR = msg({
	message: 'Cancel',
	comment: 'Short label in the desktop settings tab. Keep it concise.',
});
const DesktopSettingsTab: React.FC = observer(() => {
	const {i18n} = useLingui();
	const cachedAutostart = getCachedAutostartStatus();
	const cachedWindowBehavior = getCachedDesktopWindowBehaviorSettings();
	const [autostartEnabled, setAutostartEnabledState] = useState(cachedAutostart ?? false);
	const [autostartBusy, setAutostartBusy] = useState(cachedAutostart === null);
	const [desktopWindowBehavior, setDesktopWindowBehaviorState] = useState<DesktopWindowBehaviorSettings | null>(
		cachedWindowBehavior,
	);
	const [desktopWindowBehaviorBusy, setDesktopWindowBehaviorBusy] = useState(cachedWindowBehavior === null);
	const [trayChangePendingRestart, setTrayChangePendingRestart] = useState(false);
	const electronApi = getElectronAPI();
	const switchInstanceUrl = electronApi?.switchInstanceUrl;
	const canSwitchInstanceUrl = typeof switchInstanceUrl === 'function';
	const [instanceUrl, setInstanceUrl] = useState(() => (isDesktop() ? window.location.origin : ''));
	const [instanceUrlError, setInstanceUrlError] = useState<string | null>(null);
	const [instanceUrlBusy, setInstanceUrlBusy] = useState(false);
	const platform = guessPlatform();
	const isMac = isNativeMacOS(platform);
	const isLinux = isNativeLinux(platform);
	useLayoutEffect(() => {
		let mounted = true;
		const initDesktopSettings = async () => {
			if (!isDesktop()) {
				if (mounted) {
					setAutostartBusy(false);
					setDesktopWindowBehaviorBusy(false);
				}
				return;
			}
			const needsAutostart = getCachedAutostartStatus() === null;
			const needsWindowBehavior = getCachedDesktopWindowBehaviorSettings() === null;
			if (!needsAutostart && !needsWindowBehavior) return;
			const [enabled, windowBehavior] = await Promise.all([
				needsAutostart ? getAutostartStatus() : Promise.resolve(getCachedAutostartStatus()),
				needsWindowBehavior
					? getDesktopWindowBehaviorSettings()
					: Promise.resolve(getCachedDesktopWindowBehaviorSettings()),
			]);
			if (!mounted) return;
			if (enabled !== null) {
				setAutostartEnabledState(enabled);
			}
			if (windowBehavior !== null) {
				setDesktopWindowBehaviorState(windowBehavior);
			}
			setAutostartBusy(false);
			setDesktopWindowBehaviorBusy(false);
		};
		void initDesktopSettings();
		void getDesktopWindowBehaviorPendingRestart().then((pending) => {
			if (mounted) setTrayChangePendingRestart(pending);
		});
		return () => {
			mounted = false;
		};
	}, []);
	const handleAutostartChange = async (value: boolean) => {
		setAutostartBusy(true);
		const nextState = await setAutostartEnabled(value);
		if (nextState !== null) {
			setAutostartEnabledState(nextState);
		}
		setAutostartBusy(false);
	};
	const handleDesktopWindowBehaviorChange = async (settings: Partial<DesktopWindowBehaviorSettings>) => {
		setDesktopWindowBehaviorBusy(true);
		const nextSettings = await setDesktopWindowBehaviorSettings(settings);
		if (nextSettings !== null) {
			setDesktopWindowBehaviorState(nextSettings);
		}
		const pending = await getDesktopWindowBehaviorPendingRestart();
		setTrayChangePendingRestart(pending);
		setDesktopWindowBehaviorBusy(false);
		return {pending};
	};
	const handleInstanceUrlChange = (e: React.ChangeEvent<HTMLInputElement>) => {
		setInstanceUrl(e.target.value);
		setInstanceUrlError(null);
	};
	const handleSwitchInstance = useCallback(() => {
		if (!switchInstanceUrl) return;
		let normalized: string;
		try {
			normalized = normalizeInstanceOrigin(instanceUrl);
		} catch {
			setInstanceUrlError(
				i18n._(INVALID_INSTANCE_URL_DESCRIPTOR, {exampleDomain: EXAMPLE_DOMAIN, exampleUrl: EXAMPLE_URL}),
			);
			return;
		}
		if (normalized === window.location.origin) return;
		ModalCommands.push(
			modal(() => (
				<ConfirmModal
					title={i18n._(SWITCH_INSTANCE_CONFIRM_TITLE_DESCRIPTOR)}
					description={i18n._(SWITCH_INSTANCE_CONFIRM_DESCRIPTION_DESCRIPTOR, {
						productName: PRODUCT_NAME,
						instanceUrl: normalized,
					})}
					primaryText={i18n._(SWITCH_INSTANCE_DESCRIPTOR)}
					primaryVariant="primary"
					secondaryText={i18n._(CANCEL_DESCRIPTOR)}
					onPrimary={async () => {
						setInstanceUrlBusy(true);
						setInstanceUrlError(null);
						try {
							await switchInstanceUrl({instanceUrl: normalized});
						} catch (error) {
							setInstanceUrlError(FormUtils.extractErrorMessage(i18n, error));
						} finally {
							setInstanceUrlBusy(false);
						}
					}}
					data-flx="user.desktop-settings-tab.switch-instance.confirm-modal"
				/>
			)),
		);
	}, [i18n, instanceUrl, switchInstanceUrl]);
	const handleShowTrayIconChange = (value: boolean) => {
		void handleDesktopWindowBehaviorChange({showTrayIcon: value}).then(({pending}) => {
			if (!pending || !isNativeLinux()) return;
			ModalCommands.push(
				modal(() => (
					<ConfirmModal
						title={i18n._(RESTART_DESCRIPTOR, {productName: PRODUCT_NAME})}
						description={
							<Trans>
								Tray icon changes only apply after a restart on Linux because the system tray protocol doesn't reliably
								let apps add or remove icons at runtime. Restart now to apply your change.
							</Trans>
						}
						primaryText={i18n._(RESTART_NOW_DESCRIPTOR)}
						primaryVariant="primary"
						secondaryText={i18n._(LATER_DESCRIPTOR)}
						onPrimary={async () => {
							await relaunchDesktopApp();
						}}
						data-flx="user.desktop-settings-tab.handle-show-tray-icon-change.confirm-modal"
					/>
				)),
			);
		});
	};
	return (
		<SettingsTabContainer data-flx="user.desktop-settings-tab.settings-tab-container">
			{canSwitchInstanceUrl && (
				<SettingsSection
					id="desktop-instance"
					title={<Trans>Instance</Trans>}
					data-flx="user.desktop-settings-tab.settings-tab-section--instance"
				>
					<Input
						label={i18n._(INSTANCE_URL_DESCRIPTOR)}
						value={instanceUrl}
						onChange={handleInstanceUrlChange}
						error={instanceUrlError ?? undefined}
						disabled={instanceUrlBusy}
						autoComplete="url"
						placeholder={EXAMPLE_DOMAIN}
						footer={
							!instanceUrlError ? (
								<p data-flx="user.desktop-settings-tab.instance-url-hint">
									{i18n._(INSTANCE_URL_HINT_DESCRIPTOR, {productName: PRODUCT_NAME})}
								</p>
							) : null
						}
						data-flx="user.desktop-settings-tab.input.instance-url-change"
					/>
					<Button
						variant="secondary"
						onClick={handleSwitchInstance}
						disabled={instanceUrlBusy || !instanceUrl.trim()}
						submitting={instanceUrlBusy}
						data-flx="user.desktop-settings-tab.button.switch-instance"
					>
						<Trans>Switch</Trans>
					</Button>
				</SettingsSection>
			)}
			<SettingsSection
				id="desktop-window"
				title={<Trans>Desktop window</Trans>}
				data-flx="user.desktop-settings-tab.settings-tab-section"
			>
				{isDesktop() && (
					<Switch
						label={i18n._(LAUNCH_PRODUCT_AT_LOGIN_DESCRIPTOR, {productName: PRODUCT_NAME})}
						value={autostartEnabled}
						disabled={autostartBusy}
						onChange={handleAutostartChange}
						data-flx="user.desktop-settings-tab.switch.autostart-change"
					/>
				)}
				{isDesktop() && desktopWindowBehavior && (
					<>
						<Switch
							label={<Trans>Remember window size and position</Trans>}
							value={desktopWindowBehavior.rememberWindowState}
							disabled={desktopWindowBehaviorBusy}
							onChange={(value) => handleDesktopWindowBehaviorChange({rememberWindowState: value})}
							data-flx="user.desktop-settings-tab.switch.desktop-window-behavior-change"
						/>
						<Switch
							label={isMac ? <Trans>Show menu bar icon</Trans> : <Trans>Show system tray icon</Trans>}
							description={
								trayChangePendingRestart && isLinux
									? i18n._(RESTART_PRODUCT_FOR_TRAY_CHANGE_DESCRIPTOR, {productName: PRODUCT_NAME})
									: undefined
							}
							value={desktopWindowBehavior.showTrayIcon}
							disabled={desktopWindowBehaviorBusy}
							onChange={handleShowTrayIconChange}
							data-flx="user.desktop-settings-tab.switch.show-tray-icon-change"
						/>
						<Switch
							label={isMac ? <Trans>Minimize to menu bar</Trans> : <Trans>Minimize to tray</Trans>}
							value={desktopWindowBehavior.minimizeToTray}
							disabled={desktopWindowBehaviorBusy || !desktopWindowBehavior.showTrayIcon}
							onChange={(value) => handleDesktopWindowBehaviorChange({minimizeToTray: value})}
							data-flx="user.desktop-settings-tab.switch.desktop-window-behavior-change--2"
						/>
						<Switch
							label={isMac ? <Trans>Close to menu bar</Trans> : <Trans>Close to tray</Trans>}
							value={desktopWindowBehavior.closeToTray}
							disabled={desktopWindowBehaviorBusy || !desktopWindowBehavior.showTrayIcon}
							onChange={(value) => handleDesktopWindowBehaviorChange({closeToTray: value})}
							data-flx="user.desktop-settings-tab.switch.desktop-window-behavior-change--3"
						/>
					</>
				)}
			</SettingsSection>
		</SettingsTabContainer>
	);
});

export default DesktopSettingsTab;
