(function(global) {
    const WebUI = global.WebUI = global.WebUI || {};
    const util = WebUI.util;

    function pillRow(values) {
        return util.make('div', { class: 'pill-list' }, values.filter(Boolean).map(function(value) {
            return util.pill(value);
        }));
    }

    function factList(items) {
        const rows = items.filter(function(item) {
            return item && item.value != null && item.value !== '';
        }).map(function(item) {
            const valueNode = item.node || util.make('div', {
                class: 'kv-value' + (item.tone ? ' text-' + item.tone : ''),
                text: String(item.value)
            });
            return util.make('div', { class: 'kv-row' }, [
                util.make('div', { class: 'kv-label', text: item.label }),
                valueNode
            ]);
        });

        if (!rows.length) {
            return util.make('div', { class: 'empty-state', text: 'No data available.' });
        }

        return util.make('div', { class: 'kv-list' }, rows);
    }

    function qosTone(state) {
        if (state === 'critical') return 'danger';
        if (state === 'degraded') return 'warning';
        return 'success';
    }

    function activeBackend(status) {
        if (!status) {
            return 'unknown';
        }
        if (status.ethernet && status.ethernet.ready) {
            return 'Ethernet';
        }
        if (status.wifi && status.wifi.sta && status.wifi.sta.connected) {
            return 'WiFi STA';
        }
        if (status.wifi && status.wifi.ap && status.wifi.ap.active) {
            return 'WiFi AP';
        }
        if (status.ethernet && status.ethernet.link_up) {
            return 'Ethernet link only';
        }
        return 'offline';
    }

    WebUI.runtime = {
        pollDelayForQos: function(state, normal, degraded, critical) {
            if (state === 'critical') return critical;
            if (state === 'degraded') return degraded;
            return normal;
        },

        renderCapabilitiesSummary: function(target, capabilities) {
            if (!target) {
                return;
            }
            if (!capabilities) {
                util.setChildren(target, util.make('div', { class: 'empty-state', text: 'Capabilities unavailable.' }));
                return;
            }

            util.setChildren(target, [
                pillRow([
                    capabilities.firmware_version || 'firmware unknown',
                    capabilities.board_name || capabilities.network_profile || 'board unknown',
                    capabilities.chip_family || 'chip unknown',
                    (capabilities.device_role || 'unknown') + ' role',
                    String(capabilities.max_ntrip_slots || 0) + ' NTRIP slot(s) max',
                    capabilities.psram_available ? 'PSRAM ready' : 'No PSRAM',
                    capabilities.ethernet_supported ? 'Ethernet board' : 'WiFi-only board',
                    capabilities.has_lora_radio ? 'LoRa build enabled' : 'LoRa none'
                ]),
                factList([
                    { label: 'Firmware', value: capabilities.firmware_version || '-' },
                    { label: 'Board', value: capabilities.board_name || '-' },
                    { label: 'Chip', value: capabilities.chip_family || '-' },
                    { label: 'Role', value: capabilities.device_role || '-' },
                    { label: 'Network profile', value: capabilities.network_profile || '-' },
                    { label: 'Configured NTRIP slots', value: capabilities.configured_ntrip_slots },
                    { label: 'Max NTRIP slots', value: capabilities.max_ntrip_slots },
                    { label: 'LoRa driver', value: capabilities.lora_driver || 'none' }
                ])
            ]);
        },

        renderPlatformSummary: function(target, capabilities, status) {
            if (!target) {
                return;
            }
            if (!capabilities) {
                util.setChildren(target, util.make('div', { class: 'empty-state', text: 'Platform status unavailable.' }));
                return;
            }

            util.setChildren(target, [
                pillRow([
                    capabilities.firmware_version || 'firmware unknown',
                    capabilities.board_name || capabilities.network_profile || 'board unknown',
                    capabilities.chip_family || 'chip unknown',
                    (capabilities.device_role || 'unknown') + ' role',
                    'Uptime ' + util.formatDuration(status && status.uptime ? status.uptime : 0)
                ]),
                factList([
                    { label: 'Firmware version', value: capabilities.firmware_version || '-' },
                    { label: 'Board / profile', value: capabilities.board_name || capabilities.network_profile || '-' },
                    { label: 'Chip family', value: capabilities.chip_family || '-' },
                    { label: 'Role', value: capabilities.device_role || '-' },
                    { label: 'Network profile', value: capabilities.network_profile || '-' },
                    { label: 'Max NTRIP slots', value: capabilities.max_ntrip_slots },
                    { label: 'PSRAM', value: capabilities.psram_available ? 'available' : 'unavailable' },
                    { label: 'Safe mode', value: capabilities.safe_mode ? 'yes' : 'no' }
                ])
            ]);
        },

        renderNetworkSummary: function(target, status) {
            if (!target) {
                return;
            }
            if (!status) {
                util.setChildren(target, util.make('div', { class: 'empty-state', text: 'Network status unavailable.' }));
                return;
            }

            const ethernet = status.ethernet || {};
            const wifi = status.wifi || {};
            const sta = wifi.sta || {};
            const ap = wifi.ap || {};

            util.setChildren(target, [
                pillRow([
                    'Backend ' + activeBackend(status),
                    ethernet.ready ? 'Ethernet ready' : (ethernet.link_up ? 'Ethernet link up' : 'Ethernet idle'),
                    sta.connected ? 'WiFi STA connected' : (sta.active ? 'WiFi STA active' : 'WiFi STA off'),
                    ap.active ? 'AP active' : 'AP off'
                ]),
                factList([
                    { label: 'Active backend', value: activeBackend(status) },
                    { label: 'Ethernet link', value: ethernet.supported ? (ethernet.link_up ? 'up' : 'down') : 'not supported' },
                    { label: 'Ethernet IP state', value: ethernet.supported ? (ethernet.has_ip ? 'has IP' : 'waiting for IP') : '-' },
                    { label: 'Ethernet IP', value: ethernet.ip4 || '-' },
                    { label: 'WiFi STA', value: sta.active ? (sta.connected ? (sta.ssid || 'connected') : 'active, not connected') : 'disabled' },
                    { label: 'WiFi STA IP', value: sta.ip4 || '-' },
                    { label: 'WiFi AP', value: ap.active ? ((ap.ssid || 'active') + ' (' + (ap.devices || 0) + ' client(s))') : 'disabled' },
                    { label: 'WiFi AP IP', value: ap.ip4 || '-' }
                ])
            ]);
        },

        renderSystemSummary: function(target, status, runtimeData) {
            if (!target) {
                return;
            }
            if (!status) {
                util.setChildren(target, util.make('div', { class: 'empty-state', text: 'System status unavailable.' }));
                return;
            }

            const qos = status.qos || {};
            const heap = status.heap || {};
            const psram = status.psram || {};
            const buffers = status.buffers || {};
            const runtime = runtimeData && runtimeData.runtime ? runtimeData.runtime : {};

            util.setChildren(target, [
                pillRow([
                    'QoS ' + (qos.state || 'unknown'),
                    'Sockets ' + String(status.active_socket_count || 0) + '/' + String(status.max_socket_count || 0),
                    'Heap ' + util.formatBytes(heap.free || 0) + ' free',
                    psram.available ? ('PSRAM ' + util.formatBytes(psram.free || 0) + ' free') : 'No PSRAM'
                ]),
                factList([
                    { label: 'Free heap', value: util.formatBytes(heap.free || 0) },
                    { label: 'Min heap', value: util.formatBytes(heap.min_free || 0) },
                    { label: 'Total heap', value: util.formatBytes(heap.total || 0) },
                    { label: 'QoS state', value: qos.state || '-', tone: qosTone(qos.state) },
                    { label: 'QoS reason', value: qos.reason || '-' },
                    { label: 'Socket count', value: String(status.active_socket_count || 0) + '/' + String(status.max_socket_count || 0) },
                    { label: 'GNSS raw buffer', value: buffers.gnss_raw ? (util.formatBytes(buffers.gnss_raw.used || 0) + ' / ' + util.formatBytes(buffers.gnss_raw.size || 0)) : '-' },
                    { label: 'NTRIP ringbuffer', value: runtime.total_ringbuffer_capacity ? (util.formatBytes(runtime.total_ringbuffer_used || 0) + ' / ' + util.formatBytes(runtime.total_ringbuffer_capacity || 0)) : '-' },
                    { label: 'Dropped RTCM packets', value: runtime.total_dropped_rtcm_packets != null ? runtime.total_dropped_rtcm_packets : '-' }
                ])
            ]);
        },

        renderFooter: function(target, status) {
            if (!target) {
                return;
            }
            if (!status) {
                util.setText(target, 'Waiting for runtime...');
                return;
            }
            const heap = status.heap || {};
            const qos = status.qos || {};
            util.setText(
                target,
                'Uptime ' + util.formatDuration(status.uptime || 0) +
                ' | Heap ' + util.formatBytes(heap.free || 0) +
                ' free | QoS ' + (qos.state || 'unknown')
            );
        },

        renderAdvancedSummary: function(target, status, runtimeData, diagnostics) {
            if (!target) {
                return;
            }
            if (!status) {
                util.setChildren(target, util.make('div', { class: 'empty-state', text: 'Advanced diagnostics unavailable.' }));
                return;
            }

            const runtime = runtimeData && runtimeData.runtime ? runtimeData.runtime : {};
            const gnss = status.gnss || {};
            const diag = diagnostics || {};

            util.setChildren(target, [
                pillRow([
                    'QoS ' + ((status.qos && status.qos.state) || 'unknown'),
                    'NTRIP active ' + String(runtime.active_slot_count || 0),
                    gnss.profile ? ('GNSS ' + gnss.profile) : 'GNSS unknown',
                    diag.state ? ('Diagnostics ' + diag.state) : 'Diagnostics unknown'
                ]),
                factList([
                    { label: 'QoS reason', value: status.qos ? status.qos.reason : '-' },
                    { label: 'Ethernet ready', value: runtime.ethernet_ready ? 'yes' : 'no' },
                    { label: 'WiFi ready', value: runtime.wifi_ready ? 'yes' : 'no' },
                    { label: 'Active sockets', value: String(runtime.active_socket_count || 0) + '/' + String(runtime.max_socket_count || 0) },
                    { label: 'NTRIP active slots', value: runtime.active_slot_count || 0 },
                    { label: 'Fake RTCM', value: runtime.fake_rtcm_enabled ? 'enabled' : 'disabled' },
                    { label: 'Parser errors', value: gnss.parser_errors != null ? gnss.parser_errors : '-' },
                    { label: 'Last command status', value: gnss.last_command_status || '-' },
                    { label: 'Antenna', value: diag.antenna_status || gnss.antenna_status || '-' },
                    { label: 'Jamming', value: diag.jamming_status || gnss.jamming_status || '-' }
                ])
            ]);
        }
    };
})(window);
