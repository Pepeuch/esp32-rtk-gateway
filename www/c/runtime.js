(function(global) {
    const app = global.ConfigPage || (global.ConfigPage = {});

    app.runtimeStatus = {
        init: function() {
            this.deviceUptimeText = $('footer .uptime');
            this.deviceHeapText = $('footer .heap');
            this.wifiApStatusText = app.form.find('.wifi-ap-status');
            this.wifiStaStatusText = app.form.find('.wifi-sta-status');
            this.capabilitiesSummary = $('.capabilities-summary');
            this.streamStatsTexts = app.form.find('.stream-stats');

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

        renderCapabilities: function(cap) {
            this.capabilitiesSummary.empty();
            if (!cap) {
                this.capabilitiesSummary.text('Capabilities unavailable');
                return;
            }

            this.capabilitiesSummary
                .append($('<span>', { class: 'capability-pill', text: cap.chip_family }))
                .append($('<span>', { class: 'capability-pill', text: cap.network_profile }))
                .append($('<span>', { class: 'capability-pill', text: 'Role ' + (cap.device_role || 'unknown') }))
                .append($('<span>', { class: 'capability-pill', text: cap.max_ntrip_slots + ' NTRIP slot(s) max' }))
                .append($('<span>', { class: 'capability-pill', text: cap.psram_available ? 'PSRAM' : 'No PSRAM' }))
                .append($('<span>', { class: 'capability-pill', text: cap.ethernet_active ? 'Ethernet active' : 'Wi-Fi only' }))
                .append($('<span>', { class: 'capability-pill', text: cap.has_lora_radio ? 'LoRa available' : 'LoRa unavailable' }));
        },

        renderWifiStatus: function(wifi) {
            this.wifiApStatusText.empty();
            this.wifiStaStatusText.empty();

            if (!wifi.ap.active) {
                this.wifiApStatusText.text('Disabled');
            } else {
                this.wifiApStatusText.appendText(wifi.ap.ssid + (wifi.ap.authmode === 'OPEN' ? ' (OPEN)' : ''))
                    .appendText(' / ')
                    .append($('<a>', { text: wifi.ap.ip4, href: 'http://' + wifi.ap.ip4 + '/' }))
                    .appendText(' / ')
                    .appendText(wifi.ap.devices + ' device' + (wifi.ap.devices !== 1 ? 's' : ''));
            }

            if (!wifi.sta.active) {
                this.wifiStaStatusText.text('Not Active');
            } else if (!wifi.sta.connected) {
                this.wifiStaStatusText.text('Not Connected');
            } else {
                this.wifiStaStatusText.appendText(wifi.sta.ssid + (wifi.sta.authmode === 'OPEN' ? ' (OPEN)' : ''))
                    .appendText(' / ')
                    .append($('<a>', { text: wifi.sta.ip4, href: 'http://' + wifi.sta.ip4 + '/' }))
                    .appendText(' / ')
                    .append($('<span>', { class: 'text-' + app.utils.wifiRssiColorClass(wifi.sta.rssi), text: wifi.sta.rssi + 'dBm' }));
            }
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
                url: 'status',
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
                    self.renderCapabilities(data.capabilities);
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
        }
    };
})(window);
