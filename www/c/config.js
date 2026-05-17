(function(global) {
    const WebUI = global.WebUI = global.WebUI || {};
    const util = WebUI.util;
    const api = WebUI.api;

    function parseIpInput(value) {
        const parts = util.splitIpString(value);
        if (parts.some(function(part) { return part === ''; })) {
            return null;
        }
        return parts;
    }

    function setSecretInput(input, rawValue) {
        if (!input) {
            return;
        }
        input.value = '';
        const stored = rawValue === WebUI.constants.SECRET_UNCHANGED;
        input.dataset.secretStored = stored ? '1' : '0';
        input.placeholder = stored ? 'Stored password' : '';
    }

    function secretValue(input) {
        const value = String(input.value || '');
        if (value) {
            return value;
        }
        return input.dataset.secretStored === '1' ? WebUI.constants.SECRET_UNCHANGED : '';
    }

    const configPage = {
        state: {
            capabilities: null,
            status: null,
            config: null,
            gnssStatus: null,
            gnssProfiles: null,
            baseStatus: null,
            ntripConfig: null,
            ntripRuntime: null,
            running: false
        },

        init: function() {
            this.refs = {
                capabilitySummary: util.q('#config-capabilities'),
                networkForm: util.q('#network-config-form'),
                networkStatus: util.q('#network-save-status'),
                staEnabled: util.q('#cfg-wifi-sta-enabled'),
                staSsid: util.q('#cfg-wifi-sta-ssid'),
                staPassword: util.q('#cfg-wifi-sta-password'),
                staScan: util.q('#cfg-wifi-sta-scan'),
                staScanResults: util.q('#wifi-scan-results'),
                staDhcp: util.q('#cfg-wifi-sta-dhcp'),
                staStaticFields: util.q('#cfg-wifi-sta-static-fields'),
                staIp: util.q('#cfg-wifi-sta-ip'),
                staGateway: util.q('#cfg-wifi-sta-gateway'),
                staSubnet: util.q('#cfg-wifi-sta-subnet'),
                staDnsA: util.q('#cfg-wifi-sta-dns-a'),
                staDnsB: util.q('#cfg-wifi-sta-dns-b'),
                apEnabled: util.q('#cfg-wifi-ap-enabled'),
                apSsid: util.q('#cfg-wifi-ap-ssid'),
                apHidden: util.q('#cfg-wifi-ap-hidden'),
                apSecurity: util.q('#cfg-wifi-ap-security'),
                apPassword: util.q('#cfg-wifi-ap-password'),
                apPasswordRow: util.q('#cfg-wifi-ap-password-row'),
                apGateway: util.q('#cfg-wifi-ap-gateway'),
                apSubnet: util.q('#cfg-wifi-ap-subnet'),
                adminAuth: util.q('#cfg-admin-auth'),
                adminUser: util.q('#cfg-admin-user'),
                adminPass: util.q('#cfg-admin-pass'),
                adminCreds: util.q('#cfg-admin-credentials'),
                ethernetStatus: util.q('#cfg-ethernet-status'),
                gnssSummary: util.q('#cfg-gnss-summary'),
                gnssProfileStatus: util.q('#cfg-gnss-profile-status'),
                gnssDetect: util.q('#gnss-detect'),
                gnssProfile: util.q('#gnss-profile'),
                gnssBaud: util.q('#gnss-baud'),
                gnssNmeaRate: util.q('#gnss-nmea-rate'),
                gnssRtkTimeout: util.q('#gnss-rtk-timeout'),
                gnssDgpsTimeout: util.q('#gnss-dgps-timeout'),
                gnssConstellationMask: util.q('#gnss-constellation-mask'),
                gnssSignalMask: util.q('#gnss-signal-mask'),
                gnssRtcmOutput: util.q('#gnss-rtcm-output'),
                gnssAgnssEnable: util.q('#gnss-agnss-enable'),
                gnssApply: util.q('#gnss-profile-apply'),
                gnssStatus: util.q('#gnss-config-status'),
                baseSummary: util.q('#cfg-base-summary'),
                baseModeNote: util.q('#base-mode-note'),
                baseLatitude: util.q('#base-latitude'),
                baseLongitude: util.q('#base-longitude'),
                baseAltitude: util.q('#base-altitude-mm'),
                baseSurveyDuration: util.q('#base-survey-duration'),
                baseSurveyAccuracy: util.q('#base-survey-accuracy'),
                baseRtcmOutput: util.q('#base-rtcm-output'),
                baseStartSurvey: util.q('#base-start-survey'),
                baseStopSurvey: util.q('#base-stop-survey'),
                baseApplyFixed: util.q('#base-apply-fixed'),
                baseClear: util.q('#base-clear'),
                baseStatus: util.q('#base-config-status'),
                ntripSummary: util.q('#cfg-ntrip-summary'),
                ntripEditor: util.q('#ntrip-slot-editor'),
                ntripSave: util.q('#save-ntrip-slots'),
                ntripRestart: util.q('#restart-ntrip-runtime'),
                ntripStatus: util.q('#ntrip-save-status'),
                loraPanel: util.q('#cfg-lora-status')
            };

            this.bindEvents();
            this.state.running = true;
            this.loadAll();
            this.pollLoop();
        },

        bindEvents: function() {
            const self = this;

            this.refs.networkForm.addEventListener('submit', function(event) {
                event.preventDefault();
                self.saveNetworkConfig();
            });

            this.refs.staScan.addEventListener('click', function() {
                self.scanWifi();
            });

            this.refs.staDhcp.addEventListener('change', function() {
                self.updateVisibility();
            });

            this.refs.apSecurity.addEventListener('change', function() {
                self.updateVisibility();
            });

            this.refs.adminAuth.addEventListener('change', function() {
                self.updateVisibility();
            });

            this.refs.gnssDetect.addEventListener('click', function() {
                self.detectGnss();
            });

            this.refs.gnssApply.addEventListener('click', function() {
                self.applyGnssProfile();
            });

            this.refs.baseStartSurvey.addEventListener('click', function() {
                self.startSurvey();
            });

            this.refs.baseStopSurvey.addEventListener('click', function() {
                self.stopSurvey();
            });

            this.refs.baseApplyFixed.addEventListener('click', function() {
                self.applyFixedBase();
            });

            this.refs.baseClear.addEventListener('click', function() {
                self.clearBase();
            });

            this.refs.ntripSave.addEventListener('click', function() {
                self.saveNtrip();
            });

            this.refs.ntripRestart.addEventListener('click', function() {
                self.restartNtrip();
            });
        },

        pollLoop: async function() {
            while (this.state.running) {
                await new Promise(function(resolve) { global.setTimeout(resolve, 8000); });
                await this.refreshRuntime();
            }
        },

        loadAll: async function() {
            await this.refreshRuntime();
            this.state.config = await api.tryGet('/config', this.state.config);
            this.state.gnssProfiles = await api.tryGet('/api/gnss/profiles', this.state.gnssProfiles);
            this.state.baseStatus = await api.tryGet('/api/gnss/base/status', this.state.baseStatus);
            this.state.ntripConfig = await api.tryGet('/api/ntrip', this.state.ntripConfig);
            this.state.ntripRuntime = await api.tryGet('/api/ntrip/runtime', this.state.ntripRuntime);
            this.render();
        },

        refreshRuntime: async function() {
            const status = await api.tryGet('/status', this.state.status);
            if (status) {
                this.state.status = status;
                this.state.capabilities = status.capabilities || this.state.capabilities;
                this.state.gnssStatus = status.gnss || this.state.gnssStatus;
            }

            const baseStatus = await api.tryGet('/api/gnss/base/status', this.state.baseStatus);
            if (baseStatus) {
                this.state.baseStatus = baseStatus;
            }

            const ntripRuntime = await api.tryGet('/api/ntrip/runtime', this.state.ntripRuntime);
            if (ntripRuntime) {
                this.state.ntripRuntime = ntripRuntime;
            }

            this.renderRuntimeOnly();
        },

        render: function() {
            this.loadConfigIntoForm();
            this.renderRuntimeOnly();
            if (this.state.gnssProfiles) {
                WebUI.gnss.populateProfileForm(document, this.state.gnssProfiles, this.state.gnssStatus);
                WebUI.gnss.populateBaseForm(document, this.state.gnssProfiles, this.state.baseStatus);
            }
            if (this.state.ntripConfig) {
                WebUI.ntrip.renderSlotEditor(this.refs.ntripEditor, this.state.ntripConfig, this.state.capabilities);
            }
        },

        renderRuntimeOnly: function() {
            WebUI.runtime.renderCapabilitiesSummary(this.refs.capabilitySummary, this.state.capabilities);
            WebUI.runtime.renderNetworkSummary(this.refs.ethernetStatus, this.state.status);
            WebUI.gnss.renderOverview(this.refs.gnssSummary, this.state.gnssStatus);
            WebUI.gnss.renderProfileStatus(this.refs.gnssProfileStatus, this.state.gnssProfiles, this.state.gnssStatus);
            WebUI.gnss.renderBaseStatus(this.refs.baseSummary, this.state.baseStatus);
            WebUI.ntrip.renderRuntimeSummary(this.refs.ntripSummary, this.state.ntripRuntime || this.state.ntripConfig, this.state.capabilities);
            WebUI.lora.renderReadOnly(this.refs.loraPanel, this.state.capabilities, { context: 'config' });
        },

        loadConfigIntoForm: function() {
            const config = this.state.config;
            if (!config) {
                return;
            }

            this.refs.staEnabled.checked = config.w_sta_active === '1';
            this.refs.staSsid.value = config.w_sta_ssid || '';
            setSecretInput(this.refs.staPassword, config.w_sta_pass || '');
            this.refs.staDhcp.checked = config.w_sta_static !== '1';
            this.refs.staIp.value = util.joinIpParts(config.w_sta_ip);
            this.refs.staGateway.value = util.joinIpParts(config.w_sta_gw);
            this.refs.staSubnet.value = config.w_sta_subnet || '24';
            this.refs.staDnsA.value = util.joinIpParts(config.w_sta_dns_a);
            this.refs.staDnsB.value = util.joinIpParts(config.w_sta_dns_b);

            this.refs.apEnabled.checked = config.w_ap_active === '1';
            this.refs.apSsid.value = config.w_ap_ssid || '';
            this.refs.apHidden.checked = config.w_ap_ssid_hid === '1';
            this.refs.apSecurity.value = config.w_ap_auth_mode || '0';
            setSecretInput(this.refs.apPassword, config.w_ap_pass || '');
            this.refs.apGateway.value = util.joinIpParts(config.w_ap_gw);
            this.refs.apSubnet.value = config.w_ap_subnet || '24';

            this.refs.adminAuth.value = config.adm_auth || '0';
            this.refs.adminUser.value = config.adm_user || '';
            setSecretInput(this.refs.adminPass, config.adm_pass || '');

            this.updateVisibility();
        },

        updateVisibility: function() {
            util.show(this.refs.staStaticFields, !this.refs.staDhcp.checked);
            util.show(this.refs.apPasswordRow, this.refs.apSecurity.value !== '0');
            util.show(this.refs.adminCreds, this.refs.adminAuth.value === '2');
        },

        scanWifi: async function() {
            util.setChildren(this.refs.staScanResults, util.make('div', { class: 'small text-muted', text: 'Scanning WiFi...' }));
            const networks = await api.tryGet('/wifi/scan', null, { timeoutMs: 10000 });
            if (!networks) {
                util.setChildren(this.refs.staScanResults, util.make('div', { class: 'notice notice-warning', text: 'WiFi scan failed.' }));
                return;
            }

            util.setChildren(this.refs.staScanResults, (networks || []).map(function(entry) {
                const button = util.make('button', {
                    type: 'button',
                    class: 'btn btn-outline-secondary',
                    text: (entry.ssid || '<hidden>') + ' (' + entry.rssi + ' dBm, ' + entry.authmode + ')'
                });
                button.addEventListener('click', function() {
                    configPage.refs.staSsid.value = entry.ssid || '';
                    configPage.refs.staPassword.focus();
                });
                return button;
            }));
        },

        serializeConfigPayload: function() {
            const staIp = parseIpInput(this.refs.staIp.value);
            const staGateway = parseIpInput(this.refs.staGateway.value);
            const staDnsA = parseIpInput(this.refs.staDnsA.value);
            const staDnsB = parseIpInput(this.refs.staDnsB.value);
            const apGateway = parseIpInput(this.refs.apGateway.value);

            if (!this.refs.staDhcp.checked && (!staIp || !staGateway || !staDnsA || !staDnsB)) {
                throw new Error('Static WiFi STA fields require valid dotted IPv4 values.');
            }
            if (!apGateway) {
                throw new Error('WiFi AP gateway requires a valid dotted IPv4 value.');
            }

            const payload = {
                w_sta_active: util.toBooleanConfig(this.refs.staEnabled.checked),
                w_sta_ssid: this.refs.staSsid.value || '',
                w_sta_pass: secretValue(this.refs.staPassword),
                w_sta_scan_mode: '0',
                w_sta_static: this.refs.staDhcp.checked ? '0' : '1',
                w_ap_active: util.toBooleanConfig(this.refs.apEnabled.checked),
                w_ap_ssid: this.refs.apSsid.value || '',
                w_ap_ssid_hid: util.toBooleanConfig(this.refs.apHidden.checked),
                w_ap_auth_mode: this.refs.apSecurity.value || '0',
                w_ap_pass: secretValue(this.refs.apPassword),
                w_ap_gw: apGateway,
                w_ap_subnet: this.refs.apSubnet.value || '24',
                adm_auth: this.refs.adminAuth.value || '0',
                adm_user: this.refs.adminUser.value || '',
                adm_pass: secretValue(this.refs.adminPass)
            };

            if (!this.refs.staDhcp.checked) {
                payload.w_sta_ip = staIp;
                payload.w_sta_gw = staGateway;
                payload.w_sta_subnet = this.refs.staSubnet.value || '24';
                payload.w_sta_dns_a = staDnsA;
                payload.w_sta_dns_b = staDnsB;
            }

            return payload;
        },

        saveNetworkConfig: async function() {
            util.setText(this.refs.networkStatus, 'Saving /config payload...');
            try {
                const payload = this.serializeConfigPayload();
                await api.post('/config', payload, { timeoutMs: 8000 });
                util.setText(this.refs.networkStatus, 'Config saved. Device restart requested; reloading this page shortly...');
                global.setTimeout(function() {
                    global.location.reload();
                }, 10000);
            } catch (error) {
                util.setText(this.refs.networkStatus, 'Config save failed: ' + error.message);
            }
        },

        detectGnss: async function() {
            util.setText(this.refs.gnssStatus, 'Detecting GNSS receiver...');
            try {
                const status = await WebUI.gnss.detect();
                this.state.gnssStatus = status;
                util.setText(this.refs.gnssStatus, 'GNSS detect completed.');
                this.renderRuntimeOnly();
            } catch (error) {
                util.setText(this.refs.gnssStatus, 'GNSS detect failed: ' + error.message);
            }
        },

        applyGnssProfile: async function() {
            util.setText(this.refs.gnssStatus, 'Applying GNSS profile...');
            try {
                const response = await WebUI.gnss.applyProfile({
                    profile: this.refs.gnssProfile.value,
                    persist: true,
                    receiver_baud: util.toNumber(this.refs.gnssBaud.value, 115200),
                    nmea_rate_hz: util.toNumber(this.refs.gnssNmeaRate.value, 1),
                    rtk_timeout: util.toNumber(this.refs.gnssRtkTimeout.value, 60),
                    dgps_timeout: util.toNumber(this.refs.gnssDgpsTimeout.value, 120),
                    constellation_mask: util.toNumber(this.refs.gnssConstellationMask.value, 0),
                    signal_mask: this.refs.gnssSignalMask.value || '',
                    rtcm_output: !!this.refs.gnssRtcmOutput.checked,
                    agnss_enable: !!this.refs.gnssAgnssEnable.checked
                });
                this.state.gnssStatus = response;
                util.setText(this.refs.gnssStatus, response.warning || 'GNSS profile apply queued.');
                await this.loadAll();
            } catch (error) {
                util.setText(this.refs.gnssStatus, 'GNSS profile apply failed: ' + error.message);
            }
        },

        startSurvey: async function() {
            util.setText(this.refs.baseStatus, 'Starting survey-in...');
            try {
                this.state.baseStatus = await WebUI.gnss.startSurvey({
                    duration_s: util.toNumber(this.refs.baseSurveyDuration.value, 300),
                    accuracy_mm: util.toNumber(this.refs.baseSurveyAccuracy.value, 5000),
                    rtcm_output: !!this.refs.baseRtcmOutput.checked
                });
                util.setText(this.refs.baseStatus, this.state.baseStatus.warning || 'Survey-in started.');
                this.renderRuntimeOnly();
            } catch (error) {
                util.setText(this.refs.baseStatus, 'Start survey failed: ' + error.message);
            }
        },

        stopSurvey: async function() {
            util.setText(this.refs.baseStatus, 'Stopping survey-in...');
            try {
                this.state.baseStatus = await WebUI.gnss.stopSurvey();
                util.setText(this.refs.baseStatus, 'Survey-in stop requested.');
                this.renderRuntimeOnly();
            } catch (error) {
                util.setText(this.refs.baseStatus, 'Stop survey failed: ' + error.message);
            }
        },

        applyFixedBase: async function() {
            util.setText(this.refs.baseStatus, 'Applying fixed base position...');
            try {
                this.state.baseStatus = await WebUI.gnss.applyFixed({
                    latitude: util.toNumber(this.refs.baseLatitude.value, 0),
                    longitude: util.toNumber(this.refs.baseLongitude.value, 0),
                    altitude_mm: util.toNumber(this.refs.baseAltitude.value, 0),
                    rtcm_output: !!this.refs.baseRtcmOutput.checked
                });
                util.setText(this.refs.baseStatus, this.state.baseStatus.warning || 'Fixed base apply queued.');
                this.renderRuntimeOnly();
            } catch (error) {
                util.setText(this.refs.baseStatus, 'Apply fixed base failed: ' + error.message);
            }
        },

        clearBase: async function() {
            util.setText(this.refs.baseStatus, 'Clearing base configuration...');
            try {
                this.state.baseStatus = await WebUI.gnss.clearBase();
                util.setText(this.refs.baseStatus, 'Base configuration cleared.');
                this.renderRuntimeOnly();
            } catch (error) {
                util.setText(this.refs.baseStatus, 'Clear base failed: ' + error.message);
            }
        },

        saveNtrip: async function() {
            if (await WebUI.ntrip.saveEditor(this.refs.ntripEditor, this.refs.ntripStatus)) {
                this.state.ntripConfig = await api.tryGet('/api/ntrip', this.state.ntripConfig);
                this.state.ntripRuntime = await api.tryGet('/api/ntrip/runtime', this.state.ntripRuntime);
                this.render();
            }
        },

        restartNtrip: async function() {
            if (await WebUI.ntrip.restartRuntime(this.refs.ntripStatus)) {
                this.state.ntripRuntime = await api.tryGet('/api/ntrip/runtime', this.state.ntripRuntime);
                this.renderRuntimeOnly();
            }
        }
    };

    WebUI.configPage = configPage;
})(window);
