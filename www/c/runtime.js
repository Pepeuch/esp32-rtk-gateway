(function(global) {
    const app = global.WebUI || global.ConfigPage || {};
    global.WebUI = app;
    global.ConfigPage = app;

    app.state = app.state || {
        currentQosState: 'normal',
        nextGnssRefreshAt: 0,
        reloadOnStatus: false,
        capabilities: null
    };

    app.utils = app.utils || {};

    if (typeof app.utils.printEscape !== 'function') {
        app.utils.printEscape = function(str) {
            return String(str || '')
                .replace(/\\/g, '\\\\')
                .replace(/\n/g, '\\n')
                .replace(/\r/g, '\\r')
                .replace(/\t/g, '\\t');
        };
    }

    if (typeof app.utils.printUnescape !== 'function') {
        app.utils.printUnescape = function(str) {
            return String(str || '')
                .replace(/\\\\/g, '\\')
                .replace(/\\n/g, '\n')
                .replace(/\\r/g, '\r')
                .replace(/\\t/g, '\t');
        };
    }

    if (typeof app.utils.secondsToHHMMSS !== 'function') {
        app.utils.secondsToHHMMSS = function(seconds) {
            let value = parseInt(seconds || 0, 10);
            const hours = Math.floor(value / 3600);
            const minutes = Math.floor(value / 60) % 60;
            value %= 60;
            return [hours, minutes, value].map((entry) => entry < 10 ? '0' + entry : '' + entry).join(':');
        };
    }

    if (typeof app.utils.secondsToShort !== 'function') {
        app.utils.secondsToShort = function(seconds) {
            let value = parseInt(seconds || 0, 10);
            const hours = Math.floor(value / 3600);
            const minutes = Math.floor((value % 3600) / 60);
            value %= 60;
            return [hours, minutes, value].map((entry) => entry < 10 ? '0' + entry : '' + entry).join(':');
        };
    }

    if (typeof app.utils.humanDataSize !== 'function') {
        app.utils.humanDataSize = function(bytes) {
            let value = Number(bytes || 0);
            const threshold = 1000;
            const units = ['kB', 'MB', 'GB', 'TB', 'PB', 'EB', 'ZB', 'YB'];
            let unit = -1;

            if (Math.abs(value) < threshold) {
                return value + 'B';
            }

            do {
                value /= threshold;
                unit++;
            } while (Math.abs(value) >= threshold && unit < units.length - 1);

            return value.toFixed(1) + units[unit];
        };
    }

    if (typeof app.utils.pollDelayForQos !== 'function') {
        app.utils.pollDelayForQos = function(state, normal, degraded, critical) {
            if (state === 'critical') return critical;
            if (state === 'degraded') return degraded;
            return normal;
        };
    }

    if (typeof app.utils.wifiRssiColorClass !== 'function') {
        app.utils.wifiRssiColorClass = function(rssi) {
            if (rssi > -50) return 'primary';
            if (rssi > -60) return 'success';
            if (rssi > -70) return 'warning';
            return 'danger';
        };
    }

    if (typeof app.utils.appendText !== 'function') {
        app.utils.appendText = function(target, text) {
            $(target).append(document.createTextNode(String(text)));
            return $(target);
        };
    }

    if (typeof $.fn.appendText !== 'function') {
        $.fn.appendText = function(text) {
            return app.utils.appendText(this, text);
        };
    }

    function appendCapabilityPill(container, text) {
        if (!container || !container.length || !text) return;
        container.append($('<span>', { class: 'capability-pill', text: text }));
    }

    function renderMuted(container, text) {
        if (!container || !container.length) return;
        container.empty().append($('<div>', { class: 'small text-muted', text: text }));
    }

    function renderCapabilitiesInto(container, cap, qos) {
        if (!container || !container.length) return;
        container.empty();

        if (!cap) {
            renderMuted(container, 'Capabilities unavailable');
            return;
        }

        appendCapabilityPill(container, cap.chip_family || 'unknown');
        appendCapabilityPill(container, cap.network_profile || 'unknown network');
        appendCapabilityPill(container, 'Role ' + (cap.device_role || 'unknown'));
        appendCapabilityPill(container, (cap.max_ntrip_slots || 0) + ' NTRIP slot(s) max');
        appendCapabilityPill(container, cap.psram_available ? 'PSRAM' : 'No PSRAM');
        appendCapabilityPill(container, cap.ethernet_active ? 'Ethernet active' : 'Wi-Fi only');
        appendCapabilityPill(container, cap.has_lora_radio ? 'LoRa available' : 'LoRa unavailable');

        if (cap.has_lora_radio) {
            appendCapabilityPill(container, cap.lora_tx_enabled ? 'LoRa TX enabled' : 'LoRa TX disabled');
        }

        if (qos && qos.state) {
            appendCapabilityPill(container, 'QoS ' + qos.state);
            if (qos.reason && qos.reason !== 'normal') {
                appendCapabilityPill(container, qos.reason);
            }
        }
    }

    function renderWifiConfigStatus(apContainer, staContainer, wifi) {
        if (!apContainer.length || !staContainer.length) return;

        apContainer.empty();
        staContainer.empty();

        if (!wifi || !wifi.ap || !wifi.sta) {
            apContainer.text('Unavailable');
            staContainer.text('Unavailable');
            return;
        }

        if (!wifi.ap.active) {
            apContainer.text('Disabled');
        } else {
            apContainer.appendText(wifi.ap.ssid + (wifi.ap.authmode === 'OPEN' ? ' (OPEN)' : ''))
                .appendText(' / ')
                .append($('<a>', { text: wifi.ap.ip4 || 'no IP', href: wifi.ap.ip4 ? 'http://' + wifi.ap.ip4 + '/' : '#' }))
                .appendText(' / ')
                .appendText((wifi.ap.devices || 0) + ' device' + ((wifi.ap.devices || 0) !== 1 ? 's' : ''));
        }

        if (!wifi.sta.active) {
            staContainer.text('Not Active');
        } else if (!wifi.sta.connected) {
            staContainer.text('Not Connected');
        } else {
            staContainer.appendText(wifi.sta.ssid + (wifi.sta.authmode === 'OPEN' ? ' (OPEN)' : ''))
                .appendText(' / ')
                .append($('<a>', { text: wifi.sta.ip4 || 'no IP', href: wifi.sta.ip4 ? 'http://' + wifi.sta.ip4 + '/' : '#' }))
                .appendText(' / ')
                .append($('<span>', {
                    class: 'text-' + app.utils.wifiRssiColorClass(wifi.sta.rssi),
                    text: wifi.sta.rssi + 'dBm'
                }));
        }
    }

    function renderDashboardNetwork(container, wifi, qos, capabilities) {
        if (!container || !container.length) return;
        container.empty();

        if (!wifi) {
            renderMuted(container, 'Network status unavailable');
            return;
        }

        appendCapabilityPill(container, capabilities && capabilities.ethernet_active ? 'Ethernet active' : 'Ethernet inactive');
        if (qos && typeof qos.ethernet_ready === 'boolean') {
            appendCapabilityPill(container, qos.ethernet_ready ? 'Ethernet ready' : 'Ethernet not ready');
        }
        if (qos && typeof qos.wifi_ready === 'boolean') {
            appendCapabilityPill(container, qos.wifi_ready ? 'Wi-Fi ready' : 'Wi-Fi not ready');
        }

        if (wifi.ap) {
            appendCapabilityPill(container, wifi.ap.active ? ('AP ' + (wifi.ap.ssid || 'unnamed')) : 'AP disabled');
            if (wifi.ap.ip4) appendCapabilityPill(container, 'AP IP ' + wifi.ap.ip4);
            if (typeof wifi.ap.devices === 'number') appendCapabilityPill(container, wifi.ap.devices + ' AP client(s)');
        }

        if (wifi.sta) {
            appendCapabilityPill(container, wifi.sta.active ? 'STA enabled' : 'STA disabled');
            if (wifi.sta.connected) {
                appendCapabilityPill(container, 'STA ' + (wifi.sta.ssid || 'connected'));
                if (wifi.sta.ip4) appendCapabilityPill(container, 'STA IP ' + wifi.sta.ip4);
                if (typeof wifi.sta.rssi === 'number') appendCapabilityPill(container, wifi.sta.rssi + ' dBm');
            } else if (wifi.sta.active) {
                appendCapabilityPill(container, 'STA not connected');
            }
        }
    }

    function renderSystemSummary(container, data) {
        if (!container || !container.length) return;
        container.empty();

        if (!data) {
            renderMuted(container, 'System status unavailable');
            return;
        }

        appendCapabilityPill(container, 'Uptime ' + app.utils.secondsToHHMMSS(data.uptime || 0));

        if (data.heap && data.heap.total) {
            appendCapabilityPill(container, 'Heap ' + Math.round((data.heap.free || 0) / data.heap.total * 100) + '% free');
            appendCapabilityPill(container, 'Heap min ' + app.utils.humanDataSize(data.heap.min_free || 0));
        }

        if (data.psram && data.psram.available) {
            appendCapabilityPill(container, 'PSRAM ' + Math.round((data.psram.free || 0) / Math.max(data.psram.total || 1, 1) * 100) + '% free');
            appendCapabilityPill(container, 'PSRAM min ' + app.utils.humanDataSize(data.psram.min_free || 0));
        }

        if (data.qos && data.qos.state) {
            appendCapabilityPill(container, 'QoS ' + data.qos.state);
            if (data.qos.reason && data.qos.reason !== 'normal') {
                appendCapabilityPill(container, data.qos.reason);
            }
            appendCapabilityPill(container, (data.qos.active_socket_count || 0) + '/' + (data.qos.max_socket_count || 0) + ' sockets');
        }

        if (data.buffers && data.buffers.gnss_raw) {
            appendCapabilityPill(container, 'GNSS raw ' + app.utils.humanDataSize(data.buffers.gnss_raw.used || 0) + '/' + app.utils.humanDataSize(data.buffers.gnss_raw.size || 0));
        }

        if (data.buffers && data.buffers.ntrip) {
            appendCapabilityPill(container, 'NTRIP buf ' + app.utils.humanDataSize(data.buffers.ntrip.ringbuffer_used || 0) + '/' + app.utils.humanDataSize(data.buffers.ntrip.ringbuffer_capacity || 0));
            appendCapabilityPill(container, 'Dropped RTCM ' + (data.buffers.ntrip.dropped_rtcm_packets || 0));
        }
    }

    app.runtimeStatus = {
        init: function() {
            this.deviceUptimeText = $('footer .uptime');
            this.deviceHeapText = $('footer .heap');
            this.wifiApStatusText = app.form ? app.form.find('.wifi-ap-status') : $();
            this.wifiStaStatusText = app.form ? app.form.find('.wifi-sta-status') : $();
            this.capabilitiesSummary = $('.capabilities-summary');
            this.streamStatsTexts = app.form ? app.form.find('.stream-stats') : $();

            const self = this;
            $.getJSON('/api/capabilities').done(function(capabilities) {
                app.state.capabilities = capabilities;
                self.renderCapabilities(capabilities);
                app.applyRoleVisibility(capabilities);
                if (app.lora) app.lora.render(capabilities);
            });

            if (app.gnss) app.gnss.loadProfiles();
            if (app.ntrip) {
                app.ntrip.loadSlots();
                app.ntrip.refreshRuntime();
            }
            this.update();
        },

        initDashboard: function() {
            this.dashboardCapabilities = $('.capabilities-summary');
            this.dashboardNetwork = $('.dashboard-network-summary');
            this.dashboardSystem = $('.system-summary');
            this.dashboardUptime = $('footer .uptime');
            this.dashboardHeap = $('footer .heap');

            if (app.gnss && typeof app.gnss.init === 'function') app.gnss.init();
            if (app.ntrip && typeof app.ntrip.init === 'function') app.ntrip.init();

            const self = this;
            $.getJSON('/api/capabilities').done(function(capabilities) {
                app.state.capabilities = capabilities;
                renderCapabilitiesInto(self.dashboardCapabilities, capabilities, null);
            }).always(function() {
                self.updateDashboard();
                if (app.ntrip && typeof app.ntrip.refreshDashboardRuntime === 'function') {
                    app.ntrip.refreshDashboardRuntime();
                }
            });
        },

        renderCapabilities: function(cap, qos) {
            renderCapabilitiesInto(this.capabilitiesSummary, cap, qos);
        },

        renderWifiStatus: function(wifi) {
            renderWifiConfigStatus(this.wifiApStatusText, this.wifiStaStatusText, wifi);
        },

        renderStreamStats: function(streams) {
            this.streamStatsTexts.each(function() {
                const stream = $(this).data('stream');
                const stats = streams && streams[stream];
                if (!stats) return;

                $(this).text(app.utils.humanDataSize(stats.total.in) +
                    ' in (' + app.utils.humanDataSize(stats.rate.in) + '/s) / ' +
                    app.utils.humanDataSize(stats.total.out) +
                    ' out (' + app.utils.humanDataSize(stats.rate.out) + '/s)');
                $(this).prop('title', stats.total.in.toLocaleString() +
                    ' bytes in (' + (stats.rate.in * 8) + 'bps) / ' +
                    stats.total.out.toLocaleString() +
                    ' bytes out (' + (stats.rate.out * 8) + 'bps)');
            });
        },

        update: function() {
            const self = this;

            $.ajax({
                url: '/status',
                dataType: 'json',
                timeout: 2000
            }).done(function(data) {
                const now = Date.now();

                if (app.state.reloadOnStatus) window.location.reload();

                self.deviceUptimeText.text(app.utils.secondsToHHMMSS(parseInt(data.uptime, 10)));
                self.deviceHeapText.text(Math.round(data.heap.free / data.heap.total * 100) + '% free');
                self.renderStreamStats(data.streams);
                self.renderWifiStatus(data.wifi);

                if (data.capabilities) {
                    app.state.capabilities = data.capabilities;
                    self.renderCapabilities(data.capabilities, data.qos);
                    app.applyRoleVisibility(data.capabilities);
                    if (app.lora) app.lora.render(data.capabilities);
                }

                if (data.gnss && app.gnss) {
                    app.gnss.renderSummary(data.gnss);
                }

                if (data.qos && data.qos.state) {
                    app.state.currentQosState = data.qos.state;
                }

                if (now >= app.state.nextGnssRefreshAt && app.gnss) {
                    app.gnss.refreshViews();
                    app.state.nextGnssRefreshAt = now + app.utils.pollDelayForQos(app.state.currentQosState, 2500, 6000, 12000);
                }

                if (data.ntrip && Array.isArray(data.ntrip.slots) && app.ntrip) {
                    app.ntrip.renderCompactSummary(data.ntrip.slots);
                    app.ntrip.updateSlotRuntime(data.ntrip.slots);
                }
            }).always(function() {
                setTimeout(function() {
                    self.update();
                }, app.utils.pollDelayForQos(app.state.currentQosState, 2500, 5000, 9000));
            });
        },

        updateDashboard: function() {
            const self = this;

            $.ajax({
                url: '/status',
                dataType: 'json',
                timeout: 2000
            }).done(function(data) {
                const now = Date.now();

                if (data.qos && data.qos.state) {
                    app.state.currentQosState = data.qos.state;
                }

                if (data.capabilities) {
                    app.state.capabilities = data.capabilities;
                }

                self.dashboardUptime.text(app.utils.secondsToHHMMSS(parseInt(data.uptime || 0, 10)));
                if (data.heap && data.heap.total) {
                    self.dashboardHeap.text(Math.round((data.heap.free || 0) / data.heap.total * 100) + '% free');
                } else {
                    self.dashboardHeap.text('n/a');
                }

                renderCapabilitiesInto(self.dashboardCapabilities, app.state.capabilities || data.capabilities, data.qos);
                renderDashboardNetwork(self.dashboardNetwork, data.wifi, data.qos, app.state.capabilities || data.capabilities);
                renderSystemSummary(self.dashboardSystem, data);

                if (app.lora && typeof app.lora.renderStatusSummary === 'function') {
                    app.lora.renderStatusSummary(app.state.capabilities || data.capabilities, data.qos);
                }

                if (data.gnss && app.gnss) {
                    app.gnss.renderSummary(data.gnss);
                }

                if (now >= app.state.nextGnssRefreshAt && app.gnss) {
                    app.gnss.refreshDashboardViews();
                    app.state.nextGnssRefreshAt = now + app.utils.pollDelayForQos(app.state.currentQosState, 2500, 6000, 12000);
                }
            }).always(function() {
                setTimeout(function() {
                    self.updateDashboard();
                }, app.utils.pollDelayForQos(app.state.currentQosState, 2500, 5000, 9000));
            });
        }
    };
})(window);
