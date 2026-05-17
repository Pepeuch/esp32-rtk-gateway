(function(global) {
    const WebUI = global.WebUI = global.WebUI || {};
    const util = WebUI.util;
    const api = WebUI.api;

    const dashboard = {
        state: {
            status: null,
            diagnostics: null,
            satellites: null,
            baseStatus: null,
            ntripRuntime: null,
            running: false,
            lastDiagnosticsAt: 0,
            lastSatellitesAt: 0,
            lastBaseAt: 0,
            lastNtripAt: 0
        },

        init: function() {
            this.refs = {
                platform: util.q('#dashboard-platform'),
                network: util.q('#dashboard-network'),
                gnssOverview: util.q('#dashboard-gnss-overview'),
                gnssDiagnostics: util.q('#dashboard-gnss-diagnostics'),
                gnssConstellations: util.q('#dashboard-gnss-constellations'),
                gnssSatelliteTable: util.q('#dashboard-gnss-satellites tbody'),
                gnssSatelliteWrap: util.q('#dashboard-gnss-satellite-wrap'),
                gnssSatelliteStatus: util.q('#dashboard-gnss-satellite-status'),
                base: util.q('#dashboard-base'),
                ntripSummary: util.q('#dashboard-ntrip-summary'),
                ntripSlots: util.q('#dashboard-ntrip-slots'),
                lora: util.q('#dashboard-lora'),
                memory: util.q('#dashboard-memory'),
                footer: util.q('#dashboard-footer')
            };

            this.state.running = true;
            this.refreshLoop();
        },

        refreshLoop: async function() {
            while (this.state.running) {
                await this.refresh();
                const qosState = this.state.status && this.state.status.qos ? this.state.status.qos.state : 'normal';
                const delay = WebUI.runtime.pollDelayForQos(qosState, 3500, 6500, 10000);
                await new Promise(function(resolve) { global.setTimeout(resolve, delay); });
            }
        },

        refresh: async function() {
            const now = Date.now();

            const status = await api.tryGet('/status', this.state.status);
            if (status) {
                this.state.status = status;
            }

            if (!this.state.baseStatus || (now - this.state.lastBaseAt) > 5000) {
                const baseStatus = await api.tryGet('/api/gnss/base/status', this.state.baseStatus);
                if (baseStatus) {
                    this.state.baseStatus = baseStatus;
                    this.state.lastBaseAt = now;
                }
            }

            if (!this.state.ntripRuntime || (now - this.state.lastNtripAt) > 5000) {
                const ntripRuntime = await api.tryGet('/api/ntrip/runtime', this.state.ntripRuntime);
                if (ntripRuntime) {
                    this.state.ntripRuntime = ntripRuntime;
                    this.state.lastNtripAt = now;
                }
            }

            if (!this.state.diagnostics || (now - this.state.lastDiagnosticsAt) > 8000) {
                const diagnostics = await api.tryGet('/api/gnss/diagnostics', this.state.diagnostics);
                if (diagnostics) {
                    this.state.diagnostics = diagnostics;
                    this.state.lastDiagnosticsAt = now;
                }
            }

            if (!this.state.satellites || (now - this.state.lastSatellitesAt) > 10000) {
                const satellites = await api.tryGet('/api/gnss/satellites', this.state.satellites);
                if (satellites) {
                    this.state.satellites = satellites;
                    this.state.lastSatellitesAt = now;
                }
            }

            this.render();
        },

        render: function() {
            const status = this.state.status || {};
            const capabilities = status.capabilities || null;

            WebUI.runtime.renderPlatformSummary(this.refs.platform, capabilities, status);
            WebUI.runtime.renderNetworkSummary(this.refs.network, status);
            WebUI.gnss.renderOverview(this.refs.gnssOverview, status.gnss || null);
            WebUI.gnss.renderDiagnostics(this.refs.gnssDiagnostics, this.state.diagnostics);
            WebUI.gnss.renderSatellites({
                summary: this.refs.gnssConstellations,
                tableBody: this.refs.gnssSatelliteTable,
                tableWrap: this.refs.gnssSatelliteWrap,
                status: this.refs.gnssSatelliteStatus
            }, this.state.satellites);
            WebUI.gnss.renderBaseStatus(this.refs.base, this.state.baseStatus);
            WebUI.ntrip.renderDashboard(this.refs.ntripSummary, this.refs.ntripSlots, this.state.ntripRuntime, capabilities);
            WebUI.lora.renderReadOnly(this.refs.lora, capabilities, { context: 'dashboard' });
            WebUI.runtime.renderSystemSummary(this.refs.memory, status, this.state.ntripRuntime);
            WebUI.runtime.renderFooter(this.refs.footer, status);
        }
    };

    WebUI.dashboard = dashboard;
})(window);
