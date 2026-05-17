(function(global) {
    const WebUI = global.WebUI = global.WebUI || {};
    const util = WebUI.util;
    const api = WebUI.api;

    const MOCK_MODES = [
        { value: 'none', label: 'none' },
        { value: 'connect_ok', label: 'connect_ok' },
        { value: 'auth_fail', label: 'auth_fail' },
        { value: 'disconnect_after_packets', label: 'disconnect_after_packets' },
        { value: 'slow_socket', label: 'slow_socket' },
        { value: 'unreachable', label: 'unreachable' }
    ];

    const advanced = {
        state: {
            status: null,
            diagnostics: null,
            runtime: null,
            rawConsole: null,
            selftest: null,
            ntripConfig: null,
            lastDiagnosticsAt: 0,
            lastRuntimeAt: 0,
            lastRawAt: 0,
            lastSelftestAt: 0,
            running: false
        },

        init: function() {
            this.refs = {
                summary: util.q('#advanced-summary'),
                rawConsole: util.q('#advanced-raw-console'),
                rawStatus: util.q('#advanced-raw-status'),
                fakeRate: util.q('#advanced-fake-rate'),
                fakeSize: util.q('#advanced-fake-size'),
                fakeStart: util.q('#advanced-fake-start'),
                fakeStop: util.q('#advanced-fake-stop'),
                fakeStatus: util.q('#advanced-fake-status'),
                selftestStart: util.q('#advanced-selftest-start'),
                selftestRefresh: util.q('#advanced-selftest-refresh'),
                selftestResult: util.q('#advanced-selftest-result'),
                selftestStatus: util.q('#advanced-selftest-status'),
                mockSlot: util.q('#advanced-mock-slot'),
                mockMode: util.q('#advanced-mock-mode'),
                mockValue: util.q('#advanced-mock-value'),
                mockApply: util.q('#advanced-mock-apply'),
                mockStatus: util.q('#advanced-mock-status'),
                heapRefresh: util.q('#advanced-heap-refresh'),
                heapInfo: util.q('#advanced-heap-info')
            };

            this.bindEvents();
            this.populateMockModes();
            this.state.running = true;
            this.refreshLoop();
        },

        bindEvents: function() {
            const self = this;

            this.refs.fakeStart.addEventListener('click', async function() {
                util.setText(self.refs.fakeStatus, 'Starting fake RTCM...');
                try {
                    await api.post('/api/dev/fake-rtcm/start', {
                        rate_hz: util.toNumber(self.refs.fakeRate.value, 5),
                        packet_size: util.toNumber(self.refs.fakeSize.value, 180)
                    }, { timeoutMs: 5000 });
                    util.setText(self.refs.fakeStatus, 'Fake RTCM started.');
                    self.state.runtime = null;
                } catch (error) {
                    util.setText(self.refs.fakeStatus, 'Fake RTCM start failed: ' + error.message);
                }
            });

            this.refs.fakeStop.addEventListener('click', async function() {
                util.setText(self.refs.fakeStatus, 'Stopping fake RTCM...');
                try {
                    await api.post('/api/dev/fake-rtcm/stop', undefined, { timeoutMs: 5000 });
                    util.setText(self.refs.fakeStatus, 'Fake RTCM stopped.');
                    self.state.runtime = null;
                } catch (error) {
                    util.setText(self.refs.fakeStatus, 'Fake RTCM stop failed: ' + error.message);
                }
            });

            this.refs.selftestStart.addEventListener('click', async function() {
                util.setText(self.refs.selftestStatus, 'Starting self-test...');
                try {
                    await api.post('/api/dev/ntrip/selftest/start', undefined, { timeoutMs: 5000 });
                    util.setText(self.refs.selftestStatus, 'Self-test started.');
                    self.state.selftest = null;
                    await self.refreshSelftest();
                } catch (error) {
                    util.setText(self.refs.selftestStatus, 'Self-test start failed: ' + error.message);
                }
            });

            this.refs.selftestRefresh.addEventListener('click', function() {
                self.refreshSelftest();
            });

            this.refs.mockApply.addEventListener('click', async function() {
                util.setText(self.refs.mockStatus, 'Applying mock mode...');
                try {
                    await api.post('/api/dev/ntrip/mock', {
                        slot: util.toNumber(self.refs.mockSlot.value, 0),
                        mode: self.refs.mockMode.value,
                        value: util.toNumber(self.refs.mockValue.value, 0)
                    }, { timeoutMs: 5000 });
                    util.setText(self.refs.mockStatus, 'Mock mode updated.');
                    self.state.runtime = null;
                } catch (error) {
                    util.setText(self.refs.mockStatus, 'Mock mode update failed: ' + error.message);
                }
            });

            this.refs.heapRefresh.addEventListener('click', function() {
                self.refreshHeapInfo();
            });
        },

        populateMockModes: function() {
            util.setChildren(this.refs.mockMode, MOCK_MODES.map(function(mode) {
                return util.make('option', { value: mode.value, text: mode.label });
            }));
        },

        refreshLoop: async function() {
            while (this.state.running) {
                await this.refresh();
                const qosState = this.state.status && this.state.status.qos ? this.state.status.qos.state : 'normal';
                const delay = WebUI.runtime.pollDelayForQos(qosState, 5000, 8000, 12000);
                await new Promise(function(resolve) { global.setTimeout(resolve, delay); });
            }
        },

        refresh: async function() {
            const now = Date.now();
            const status = await api.tryGet('/status', this.state.status);
            if (status) {
                this.state.status = status;
            }

            if (!this.state.runtime || (now - this.state.lastRuntimeAt) > 5000) {
                const runtime = await api.tryGet('/api/ntrip/runtime', this.state.runtime);
                if (runtime) {
                    this.state.runtime = runtime;
                    this.state.lastRuntimeAt = now;
                }
            }

            if (!this.state.diagnostics || (now - this.state.lastDiagnosticsAt) > 8000) {
                const diagnostics = await api.tryGet('/api/gnss/diagnostics', this.state.diagnostics);
                if (diagnostics) {
                    this.state.diagnostics = diagnostics;
                    this.state.lastDiagnosticsAt = now;
                }
            }

            if (!this.state.rawConsole || (now - this.state.lastRawAt) > 7000) {
                const rawConsole = await api.tryGet('/api/gnss/receiver/raw', this.state.rawConsole, { timeoutMs: 5000 });
                if (rawConsole) {
                    this.state.rawConsole = rawConsole;
                    this.state.lastRawAt = now;
                }
            }

            if (!this.state.selftest || (now - this.state.lastSelftestAt) > 6000) {
                await this.refreshSelftest();
            }

            if (!this.state.ntripConfig) {
                this.state.ntripConfig = await api.tryGet('/api/ntrip', null);
            }

            this.render();
        },

        refreshSelftest: async function() {
            const result = await api.tryGet('/api/dev/ntrip/selftest/result', this.state.selftest, { timeoutMs: 5000 });
            if (result) {
                this.state.selftest = result;
                this.state.lastSelftestAt = Date.now();
            }
            this.renderSelftest();
        },

        refreshHeapInfo: async function() {
            util.setText(this.refs.heapInfo, 'Loading heap info...');
            const data = await api.tryGet('/heap_info', null, { timeoutMs: 5000 });
            if (!data) {
                util.setText(this.refs.heapInfo, 'Heap info unavailable.');
                return;
            }
            util.setText(this.refs.heapInfo, JSON.stringify(data, null, 2));
        },

        render: function() {
            const status = this.state.status || {};
            const capabilities = status.capabilities || null;

            WebUI.runtime.renderAdvancedSummary(this.refs.summary, status, this.state.runtime, this.state.diagnostics);
            WebUI.gnss.renderRawConsole(this.refs.rawConsole, this.state.rawConsole);
            util.setText(this.refs.rawStatus,
                this.state.rawConsole && this.state.rawConsole.success
                    ? 'Receiver raw console refreshed.'
                    : 'Raw console is unavailable or QoS-limited.');
            WebUI.ntrip.populateSlotSelect(this.refs.mockSlot, this.state.ntripConfig, capabilities);
            this.renderSelftest();
        },

        renderSelftest: function() {
            const data = this.state.selftest;
            if (!data) {
                util.setText(this.refs.selftestStatus, 'Self-test status unavailable.');
                util.setText(this.refs.selftestResult, '{}');
                return;
            }
            util.setText(this.refs.selftestStatus, 'Self-test state: ' + (data.state || 'unknown'));
            util.setText(this.refs.selftestResult, JSON.stringify(data, null, 2));
        }
    };

    WebUI.advanced = advanced;
})(window);
