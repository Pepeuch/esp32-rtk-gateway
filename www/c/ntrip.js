(function(global) {
    const app = global.WebUI || global.ConfigPage || {};
    global.WebUI = app;
    global.ConfigPage = app;

    const SLOT_LABELS = ['A', 'B', 'C', 'D', 'E'];

    function slotBadgeClass(status) {
        if (status === 'streaming') return 'ok';
        if (status === 'connecting' || status === 'authenticating' || status === 'reconnect_wait' || status === 'hardware_limited') return 'warn';
        if (status === 'error') return 'error';
        return 'muted';
    }

    app.ntrip = {
        init: function() {
            this.summary = $('.ntrip-slots-summary');
            this.runtimeSummary = $('.ntrip-runtime-summary');
            this.editor = $('#ntrip-slot-editor');
            this.saveButton = $('#save-ntrip-slots');
            this.dashboardRuntimeSummary = $('.dashboard-ntrip-runtime-summary');
            this.dashboardSlotList = $('.dashboard-ntrip-slot-list');
            this.lastPayload = null;
            this.bindEvents();
        },

        defaultSlotName: function(index) {
            return 'NTRIP Server ' + (SLOT_LABELS[index] || String.fromCharCode(65 + index));
        },

        fallbackMaxSlots: function(payload) {
            const stateCapabilities = app.state && app.state.capabilities ? app.state.capabilities : null;
            const payloadCapabilities = payload && payload.capabilities ? payload.capabilities : null;

            if (stateCapabilities && stateCapabilities.max_ntrip_slots) return parseInt(stateCapabilities.max_ntrip_slots, 10);
            if (payloadCapabilities && payloadCapabilities.max_ntrip_slots) return parseInt(payloadCapabilities.max_ntrip_slots, 10);
            if ((stateCapabilities && stateCapabilities.is_esp32s3) || (payloadCapabilities && payloadCapabilities.is_esp32s3)) return 5;
            return 5;
        },

        buildFallbackSlots: function(count) {
            const slots = [];
            for (let index = 0; index < count; index++) {
                slots.push({
                    index: index,
                    enabled: false,
                    name: this.defaultSlotName(index),
                    host: '',
                    port: 2101,
                    mountpoint: '',
                    username: '',
                    password: '',
                    has_password: false,
                    ntrip_version: '2.0',
                    use_tls: false,
                    status: 'idle',
                    bytes_sent: 0,
                    bytes_per_sec: 0,
                    reconnect_count: 0,
                    uptime_seconds: 0,
                    last_http_code: 0,
                    dropped_rtcm_packets: 0,
                    ringbuffer_high_water: 0,
                    mock_mode: 'none',
                    last_error: '',
                    disabled_reason: '',
                    stale: false
                });
            }
            return slots;
        },

        completeSlots: function(payload) {
            const maxSlots = this.fallbackMaxSlots(payload);
            const sourceSlots = Array.isArray(payload && payload.slots) ? payload.slots : [];
            const completed = this.buildFallbackSlots(maxSlots);

            sourceSlots.forEach(function(slot) {
                if (!slot || typeof slot.index !== 'number' || slot.index < 0 || slot.index >= completed.length) return;
                completed[slot.index] = Object.assign({}, completed[slot.index], slot, {
                    name: slot.name || completed[slot.index].name
                });
            });

            return completed;
        },

        visibleSlotCount: function(payload) {
            return this.completeSlots(payload).length;
        },

        renderRuntimeSummaryInto: function(container, runtime) {
            if (!runtime || !container.length) return;
            container.empty()
                .append($('<span>', { class: 'capability-pill', text: runtime.active_slot_count + ' active slot(s)' }))
                .append($('<span>', { class: 'capability-pill', text: app.utils.humanDataSize(runtime.free_heap_bytes || 0) + ' free heap' }))
                .append($('<span>', { class: 'capability-pill', text: app.utils.humanDataSize(runtime.min_free_heap_bytes || 0) + ' min heap' }))
                .append($('<span>', { class: 'capability-pill', text: runtime.fake_rtcm_enabled ? ('Fake RTCM ' + (runtime.fake_rtcm_rate_hz || 0) + 'Hz') : 'Fake RTCM off' }))
                .append($('<span>', { class: 'capability-pill', text: runtime.safe_mode ? 'Safe mode' : 'Normal mode' }))
                .append($('<span>', { class: 'capability-pill', text: 'QoS ' + (runtime.qos_state || 'normal') }))
                .append($('<span>', { class: 'capability-pill', text: (runtime.active_socket_count || 0) + '/' + (runtime.max_socket_count || 0) + ' sockets' }))
                .append($('<span>', { class: 'capability-pill', text: app.utils.humanDataSize(runtime.total_ringbuffer_used || 0) + '/' + app.utils.humanDataSize(runtime.total_ringbuffer_capacity || 0) + ' buffer' }))
                .append($('<span>', { class: 'capability-pill', text: (runtime.total_dropped_rtcm_packets || 0) + ' dropped packets' }));

            if (runtime.qos_reason && runtime.qos_reason !== 'normal') {
                container.append($('<span>', { class: 'capability-pill', text: runtime.qos_reason }));
            }
        },

        renderRuntimeSummary: function(runtime) {
            this.renderRuntimeSummaryInto(this.runtimeSummary, runtime);
        },

        updateSlotRuntime: function(slots) {
            const self = this;
            if (!Array.isArray(slots)) return;

            slots.forEach(function(slot) {
                const card = self.editor.find('.ntrip-slot-card[data-slot-index="' + slot.index + '"]');
                if (!card.length) return;

                card.find('.slot-name').text(slot.name);
                card.find('.slot-status').removeClass('ok warn error muted').addClass(slotBadgeClass(slot.status)).text(slot.stale && slot.status === 'streaming' ? 'stale' : slot.status);
                card.find('.slot-reason').text(slot.disabled_reason || (slot.stale ? 'No recent RTCM activity' : ''));
                card.find('.slot-bytes').text(app.utils.humanDataSize(slot.bytes_sent));
                card.find('.slot-rate').text(app.utils.humanDataSize(slot.bytes_per_sec) + '/s');
                card.find('.slot-reconnects').text(slot.reconnect_count);
                card.find('.slot-uptime').text(app.utils.secondsToShort(slot.uptime_seconds));
                card.find('.slot-http').text(slot.last_http_code || '-');
                card.find('.slot-dropped').text(slot.dropped_rtcm_packets || 0);
                card.find('.slot-high-water').text(app.utils.humanDataSize(slot.ringbuffer_high_water || 0));
                card.find('.slot-mock-current').text(slot.mock_mode || 'none');
                card.find('.slot-error').text(slot.last_error || 'None');
                card.attr('data-has-password', slot.has_password ? '1' : '0');
            });
        },

        renderSlots: function(payload) {
            const self = this;
            if (!payload) return;

            const slots = this.completeSlots(payload);
            this.lastPayload = Object.assign({}, payload, { slots: slots });
            this.editor.empty();
            slots.forEach(function(slot) {
                const card = $('<div>', {
                    class: 'card mb-3 ntrip-slot-card',
                    'data-slot-index': slot.index,
                    'data-has-password': slot.has_password ? '1' : '0'
                });

                const header = $('<div>', { class: 'card-header d-flex justify-content-between align-items-center' });
                const left = $('<div>');
                const right = $('<label>', { class: 'mb-0 small' });
                const body = $('<div>', { class: 'card-body' });
                const meta = $('<div>', { class: 'ntrip-slot-meta' });

                left.append($('<strong>', { class: 'slot-name', text: slot.name }))
                    .append(' ')
                    .append($('<span>', { class: 'slot-badge slot-status ' + slotBadgeClass(slot.status), text: slot.status }));
                right.append($('<input>', { type: 'checkbox', class: 'mr-2 slot-enabled', checked: slot.enabled }))
                    .append(document.createTextNode('Enabled'));
                header.append(left).append(right);

                body.append($('<div>', { class: 'small text-muted slot-reason mb-3', text: slot.disabled_reason || '' }));
                body.append($('<div>', { class: 'form-row mb-3' })
                    .append($('<div>', { class: 'col-md-4' }).append($('<label>', { text: 'Name' }), $('<input>', { type: 'text', class: 'form-control slot-name-input', value: slot.name, maxlength: 31 })))
                    .append($('<div>', { class: 'col-md-5' }).append($('<label>', { text: 'Host' }), $('<input>', { type: 'text', class: 'form-control slot-host', value: slot.host, maxlength: 95 })))
                    .append($('<div>', { class: 'col-md-3' }).append($('<label>', { text: 'Port' }), $('<input>', { type: 'number', class: 'form-control slot-port', value: slot.port, min: 0, max: 65535 }))));
                body.append($('<div>', { class: 'form-row mb-3' })
                    .append($('<div>', { class: 'col-md-4' }).append($('<label>', { text: 'Mountpoint' }), $('<input>', { type: 'text', class: 'form-control slot-mountpoint', value: slot.mountpoint, maxlength: 63 })))
                    .append($('<div>', { class: 'col-md-4' }).append($('<label>', { text: 'Username' }), $('<input>', { type: 'text', class: 'form-control slot-username', value: slot.username, maxlength: 63 })))
                    .append($('<div>', { class: 'col-md-4' }).append($('<label>', { text: 'Password' }), $('<input>', { type: 'password', class: 'form-control slot-password', placeholder: slot.has_password ? 'Leave empty to keep stored password' : '' }))));
                body.append($('<div>', { class: 'form-row mb-3' })
                    .append($('<div>', { class: 'col-md-4' }).append($('<label>', { text: 'NTRIP Version' }), $('<input>', { type: 'text', class: 'form-control slot-version', value: slot.ntrip_version, maxlength: 15 })))
                    .append($('<div>', { class: 'col-md-4 d-flex align-items-end' }).append($('<label>', { class: 'mb-2' }).append($('<input>', { type: 'checkbox', class: 'mr-2 slot-use-tls', checked: !!slot.use_tls }), document.createTextNode('Use TLS (future)')))));
                body.append($('<div>', { class: 'form-row mb-3' })
                    .append($('<div>', { class: 'col-md-5' }).append($('<label>', { text: 'Mock mode' }),
                        $('<select>', { class: 'form-control slot-mock-mode' })
                            .append($('<option>', { value: 'none', text: 'none' }))
                            .append($('<option>', { value: 'connect_ok', text: 'connect_ok' }))
                            .append($('<option>', { value: 'auth_fail', text: 'auth_fail' }))
                            .append($('<option>', { value: 'disconnect_after_packets', text: 'disconnect_after_packets' }))
                            .append($('<option>', { value: 'slow_socket', text: 'slow_socket' }))
                            .append($('<option>', { value: 'unreachable', text: 'unreachable' }))
                            .val(slot.mock_mode || 'none')))
                    .append($('<div>', { class: 'col-md-3' }).append($('<label>', { text: 'Mock value' }), $('<input>', { type: 'number', class: 'form-control slot-mock-value', value: slot.mock_mode_value || 0, min: 0 })))
                    .append($('<div>', { class: 'col-md-4 d-flex align-items-end' }).append($('<button>', { type: 'button', class: 'btn btn-outline-secondary w-100 slot-apply-mock', text: 'Apply Mock' }))));

                meta.append($('<div>', { html: 'Bytes sent: <strong class="slot-bytes">' + app.utils.humanDataSize(slot.bytes_sent) + '</strong>' }))
                    .append($('<div>', { html: 'Rate: <strong class="slot-rate">' + app.utils.humanDataSize(slot.bytes_per_sec || 0) + '/s</strong>' }))
                    .append($('<div>', { html: 'Reconnect count: <strong class="slot-reconnects">' + slot.reconnect_count + '</strong>' }))
                    .append($('<div>', { html: 'Uptime: <strong class="slot-uptime">' + app.utils.secondsToShort(slot.uptime_seconds || 0) + '</strong>' }))
                    .append($('<div>', { html: 'Last HTTP code: <strong class="slot-http">' + (slot.last_http_code || '-') + '</strong>' }))
                    .append($('<div>', { html: 'Dropped RTCM packets: <strong class="slot-dropped">' + (slot.dropped_rtcm_packets || 0) + '</strong>' }))
                    .append($('<div>', { html: 'Buffer high-water: <strong class="slot-high-water">' + app.utils.humanDataSize(slot.ringbuffer_high_water || 0) + '</strong>' }))
                    .append($('<div>', { html: 'Mock mode: <strong class="slot-mock-current">' + (slot.mock_mode || 'none') + '</strong>' }))
                    .append($('<div>', { html: 'Last error: <strong class="slot-error">' + (slot.last_error || 'None') + '</strong>' }));

                body.append(meta);
                card.append(header).append(body);
                self.editor.append(card);
            });
        },

        renderDashboardSlots: function(slots) {
            const container = this.dashboardSlotList;
            if (!container.length) return;
            container.empty();

            const normalized = this.completeSlots({ slots: slots });
            if (!normalized.length) {
                container.text('NTRIP runtime unavailable');
                return;
            }

            normalized.forEach(function(slot) {
                let text = (slot.index + 1) + '. ' + (slot.name || 'slot') + ' - ' + (slot.stale && slot.status === 'streaming' ? 'stale' : (slot.status || 'unknown'));
                if (slot.disabled_reason) text += ' (' + slot.disabled_reason + ')';
                text += ' - ' + app.utils.humanDataSize(slot.bytes_per_sec || 0) + '/s';
                container.append($('<div>', { class: 'small mb-1', text: text }));
            });
        },

        loadSlots: function() {
            const self = this;
            $.getJSON('api/ntrip/runtime').done(function(data) {
                self.renderSlots(data);
                self.renderRuntimeSummary(data.runtime);
            });
        },

        refreshRuntime: function() {
            const self = this;
            $.getJSON('api/ntrip/runtime').done(function(data) {
                if (data && data.slots) {
                    self.lastPayload = Object.assign({}, data, { slots: self.completeSlots(data) });
                    self.updateSlotRuntime(self.lastPayload.slots);
                }
                if (data && data.runtime) {
                    app.state.currentQosState = data.runtime.qos_state || app.state.currentQosState;
                    self.renderRuntimeSummary(data.runtime);
                }
            }).always(function() {
                setTimeout(function() {
                    self.refreshRuntime();
                }, app.utils.pollDelayForQos(app.state.currentQosState, 2500, 5000, 9000));
            });
        },

        refreshDashboardRuntime: function() {
            const self = this;
            $.getJSON('api/ntrip/runtime').done(function(data) {
                self.lastPayload = Object.assign({}, data, { slots: self.completeSlots(data) });
                if (data && data.runtime) {
                    app.state.currentQosState = data.runtime.qos_state || app.state.currentQosState;
                    self.renderRuntimeSummaryInto(self.dashboardRuntimeSummary, data.runtime);
                }
                if (self.lastPayload && self.lastPayload.slots) {
                    self.renderDashboardSlots(self.lastPayload.slots);
                }
            }).always(function() {
                setTimeout(function() {
                    self.refreshDashboardRuntime();
                }, app.utils.pollDelayForQos(app.state.currentQosState, 2500, 5000, 9000));
            });
        },

        renderCompactSummary: function(slots) {
            if (!Array.isArray(slots) || !this.summary.length) return;
            this.summary.empty().append($('<div>', { class: 'small font-weight-bold mb-2', text: 'NTRIP slots' }));

            this.completeSlots({ slots: slots }).forEach(function(slot) {
                let text = (slot.index + 1) + '. ' + slot.name + ' - ' + slot.status;
                let color = 'text-muted';

                if (slot.disabled_reason) text += ' (' + slot.disabled_reason + ')';
                if (slot.running) color = 'text-success';
                else if (slot.enabled && !slot.allowed_by_platform) color = 'text-warning';
                else if (slot.enabled) color = 'text-primary';

                app.ntrip.summary.append($('<div>', { class: 'small ' + color, text: text }));
            });
        },

        bindEvents: function() {
            const self = this;

            if (this.saveButton.length) {
                this.saveButton.on('click', function() {
                    const preserved = Array.isArray(self.lastPayload && self.lastPayload.slots)
                        ? self.lastPayload.slots.map(function(slot) {
                            return {
                                index: slot.index,
                                enabled: slot.enabled,
                                name: slot.name || self.defaultSlotName(slot.index),
                                host: slot.host,
                                port: slot.port || 2101,
                                mountpoint: slot.mountpoint,
                                username: slot.username,
                                password: '\x1a\x1a\x1a\x1a\x1a\x1a\x1a\x1a',
                                ntrip_version: slot.ntrip_version || '2.0',
                                use_tls: !!slot.use_tls
                            };
                        })
                        : self.buildFallbackSlots(5).map(function(slot) {
                            return {
                                index: slot.index,
                                enabled: slot.enabled,
                                name: slot.name,
                                host: slot.host,
                            port: slot.port,
                            mountpoint: slot.mountpoint,
                            username: slot.username,
                                password: '\x1a\x1a\x1a\x1a\x1a\x1a\x1a\x1a',
                                ntrip_version: slot.ntrip_version,
                                use_tls: !!slot.use_tls
                            };
                        });

                    self.editor.find('.ntrip-slot-card').each(function() {
                        const card = $(this);
                        const hasPassword = card.attr('data-has-password') === '1';
                        let password = card.find('.slot-password').val();

                        if (!password && hasPassword) password = '\x1a\x1a\x1a\x1a\x1a\x1a\x1a\x1a';

                        const slotData = {
                            index: parseInt(card.attr('data-slot-index'), 10),
                            enabled: card.find('.slot-enabled').is(':checked'),
                            name: card.find('.slot-name-input').val(),
                            host: card.find('.slot-host').val(),
                            port: parseInt(card.find('.slot-port').val() || '0', 10),
                            mountpoint: card.find('.slot-mountpoint').val(),
                            username: card.find('.slot-username').val(),
                            password: password || '',
                            ntrip_version: card.find('.slot-version').val(),
                            use_tls: card.find('.slot-use-tls').is(':checked')
                        };

                        preserved[slotData.index] = slotData;
                    });

                    self.saveButton.prop('disabled', true);
                    $.ajax({
                        url: 'api/ntrip',
                        method: 'POST',
                        contentType: 'application/json',
                        data: JSON.stringify({ slots: preserved })
                    }).done(function() {
                        self.loadSlots();
                    }).always(function() {
                        self.saveButton.prop('disabled', false);
                    });
                });
            }

            $('#restart-ntrip-runtime').on('click', function() {
                const button = $(this);
                button.prop('disabled', true);
                $.post('api/ntrip/restart').always(function() { button.prop('disabled', false); });
            });

            $('#fake-rtcm-start').on('click', function() {
                const button = $(this);
                button.prop('disabled', true);
                $.ajax({
                    url: 'api/dev/fake-rtcm/start',
                    method: 'POST',
                    contentType: 'application/json',
                    data: JSON.stringify({
                        rate_hz: parseInt($('#fake-rtcm-rate').val() || '5', 10),
                        packet_size: parseInt($('#fake-rtcm-size').val() || '180', 10)
                    })
                }).always(function() { button.prop('disabled', false); });
            });

            $('#fake-rtcm-stop').on('click', function() {
                const button = $(this);
                button.prop('disabled', true);
                $.post('api/dev/fake-rtcm/stop').always(function() { button.prop('disabled', false); });
            });

            this.editor.on('click', '.slot-apply-mock', function() {
                const button = $(this);
                const card = button.closest('.ntrip-slot-card');
                button.prop('disabled', true);
                $.ajax({
                    url: 'api/dev/ntrip/mock',
                    method: 'POST',
                    contentType: 'application/json',
                    data: JSON.stringify({
                        slot: parseInt(card.attr('data-slot-index'), 10),
                        mode: card.find('.slot-mock-mode').val(),
                        value: parseInt(card.find('.slot-mock-value').val() || '0', 10)
                    })
                }).always(function() { button.prop('disabled', false); });
            });
        }
    };
})(window);
