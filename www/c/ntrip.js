(function(global) {
    const WebUI = global.WebUI = global.WebUI || {};
    const util = WebUI.util;
    const api = WebUI.api;

    const SLOT_COUNT = 5;

    function factList(items) {
        const rows = items.filter(function(item) {
            return item && item.value != null && item.value !== '';
        }).map(function(item) {
            return util.make('div', { class: 'kv-row' }, [
                util.make('div', { class: 'kv-label', text: item.label }),
                util.make('div', { class: 'kv-value' + (item.tone ? ' text-' + item.tone : ''), text: String(item.value) })
            ]);
        });

        return rows.length
            ? util.make('div', { class: 'kv-list' }, rows)
            : util.make('div', { class: 'empty-state', text: 'No NTRIP data available.' });
    }

    function slotLetter(index) {
        return String.fromCharCode(65 + index);
    }

    function defaultSlot(index) {
        return {
            index: index,
            id: 'slot' + index,
            name: 'Slot ' + slotLetter(index),
            enabled: false,
            running: false,
            implemented: true,
            allowed_by_platform: true,
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
            packets_sent: 0,
            reconnect_count: 0,
            uptime_seconds: 0,
            last_activity_ms: 0,
            dropped_rtcm_packets: 0,
            ringbuffer_high_water: 0,
            ringbuffer_capacity: 0,
            ringbuffer_used: 0,
            ringbuffer_free: 0,
            last_http_code: 0,
            stale: false,
            mock_mode: 'none',
            mock_mode_value: 0,
            last_error: '',
            disabled_reason: ''
        };
    }

    function visibleCount(payload, capabilities) {
        const fromCapabilities = capabilities && capabilities.max_ntrip_slots != null
            ? Number(capabilities.max_ntrip_slots)
            : null;
        const fromPayload = payload && payload.max_slots != null
            ? Number(payload.max_slots)
            : null;
        const count = fromCapabilities || fromPayload || SLOT_COUNT;
        return Math.max(1, Math.min(SLOT_COUNT, count));
    }

    function completeSlots(payload) {
        const source = payload && Array.isArray(payload.slots) ? payload.slots : [];
        const slots = [];
        for (let index = 0; index < SLOT_COUNT; index++) {
            slots.push(Object.assign(defaultSlot(index), source[index] || source.find(function(slot) {
                return slot && Number(slot.index) === index;
            }) || {}));
        }
        return slots;
    }

    function statusTone(slot) {
        if (!slot.allowed_by_platform) return 'warning';
        if (!slot.enabled) return 'secondary';
        if (slot.status === 'streaming') return 'success';
        if (slot.status === 'error') return 'danger';
        if (slot.status === 'connecting' || slot.status === 'authenticating' || slot.status === 'reconnect_wait') return 'warning';
        return 'info';
    }

    function slotStatusText(slot) {
        if (!slot.allowed_by_platform) return 'platform limited';
        if (!slot.enabled) return 'disabled';
        return slot.status || 'idle';
    }

    function cardField(label, element) {
        return util.make('label', { class: 'field-stack' }, [
            util.make('span', { class: 'small text-muted', text: label }),
            element
        ]);
    }

    WebUI.ntrip = {
        loadConfig: function() {
            return api.get('/api/ntrip');
        },

        loadRuntime: function() {
            return api.get('/api/ntrip/runtime');
        },

        visibleCount: visibleCount,

        completeSlots: completeSlots,

        renderRuntimeSummary: function(target, payload, capabilities) {
            if (!target) {
                return;
            }
            if (!payload) {
                util.setChildren(target, util.make('div', { class: 'empty-state', text: 'NTRIP runtime unavailable.' }));
                return;
            }
            const runtime = payload.runtime || {};
            const count = visibleCount(payload, capabilities);
            util.setChildren(target, [
                util.make('div', { class: 'pill-list' }, [
                    util.pill(String(count) + ' visible slot(s)'),
                    util.pill(String(runtime.active_slot_count || 0) + ' active'),
                    util.pill(runtime.fake_rtcm_enabled ? 'Fake RTCM on' : 'Fake RTCM off'),
                    util.pill('QoS ' + (runtime.qos_state || 'unknown'))
                ]),
                factList([
                    { label: 'Visible slots', value: count },
                    { label: 'Requested enabled slots', value: payload.requested_enabled_slots != null ? payload.requested_enabled_slots : '-' },
                    { label: 'Active slots', value: runtime.active_slot_count != null ? runtime.active_slot_count : '-' },
                    { label: 'Dropped RTCM packets', value: runtime.total_dropped_rtcm_packets != null ? runtime.total_dropped_rtcm_packets : '-' },
                    { label: 'Ringbuffer high-water', value: runtime.total_ringbuffer_high_water != null ? runtime.total_ringbuffer_high_water : '-' }
                ])
            ]);
        },

        renderSlotEditor: function(target, payload, capabilities) {
            if (!target) {
                return;
            }

            const slots = completeSlots(payload);
            const count = visibleCount(payload, capabilities);

            const cards = [];
            for (let index = 0; index < count; index++) {
                const slot = slots[index];
                const passwordInput = util.make('input', {
                    type: 'password',
                    class: 'form-control',
                    value: '',
                    attrs: {
                        autocomplete: 'new-password',
                        placeholder: slot.has_password ? 'Stored password' : ''
                    }
                });

                const enabledCheckbox = util.make('input', {
                    type: 'checkbox',
                    checked: !!slot.enabled,
                    class: 'form-check-input'
                });

                const card = util.make('section', {
                    class: 'slot-card',
                    dataset: {
                        slotIndex: String(slot.index),
                        hasPassword: slot.has_password ? '1' : '0'
                    }
                }, [
                    util.make('div', { class: 'slot-card-header' }, [
                        util.make('div', {}, [
                            util.make('div', { class: 'font-weight-bold', text: 'Slot ' + slotLetter(slot.index) }),
                            util.make('div', { class: 'small text-muted', text: slotStatusText(slot) })
                        ]),
                        util.badge(slot.status || 'idle', statusTone(slot))
                    ]),
                    util.make('label', { class: 'checkbox-row' }, [
                        enabledCheckbox,
                        util.make('span', { text: 'Enabled' })
                    ]),
                    util.make('div', { class: 'field-grid' }, [
                        cardField('Name', util.make('input', { type: 'text', class: 'form-control', value: slot.name || '' })),
                        cardField('Host', util.make('input', { type: 'text', class: 'form-control', value: slot.host || '' })),
                        cardField('Port', util.make('input', { type: 'number', class: 'form-control', value: String(slot.port || 2101), attrs: { min: '0', max: '65535' } })),
                        cardField('Mountpoint', util.make('input', { type: 'text', class: 'form-control', value: slot.mountpoint || '' })),
                        cardField('Username', util.make('input', { type: 'text', class: 'form-control', value: slot.username || '' })),
                        cardField('Password', passwordInput)
                    ]),
                    factList([
                        { label: 'Allowed by platform', value: slot.allowed_by_platform ? 'yes' : 'no' },
                        { label: 'Reconnect count', value: slot.reconnect_count != null ? slot.reconnect_count : '-' },
                        { label: 'Dropped packets', value: slot.dropped_rtcm_packets != null ? slot.dropped_rtcm_packets : '-' },
                        { label: 'High-water', value: slot.ringbuffer_high_water != null ? slot.ringbuffer_high_water : '-' },
                        { label: 'Last HTTP code', value: slot.last_http_code || '-' },
                        { label: 'Last error', value: slot.last_error || slot.disabled_reason || '-' }
                    ])
                ]);

                const fields = util.qa('input', card);
                card.__fields = {
                    enabled: enabledCheckbox,
                    name: fields[1],
                    host: fields[2],
                    port: fields[3],
                    mountpoint: fields[4],
                    username: fields[5],
                    password: passwordInput
                };
                cards.push(card);
            }

            target.__ntripPayload = payload;
            target.__ntripVisibleCount = count;
            util.setChildren(target, cards);
        },

        collectSlots: function(target, payload) {
            const slots = completeSlots(payload);
            const cards = util.qa('.slot-card', target);

            cards.forEach(function(card) {
                const index = Number(card.dataset.slotIndex || '0');
                const fields = card.__fields;
                if (!fields || !slots[index]) {
                    return;
                }

                const slot = slots[index];
                slot.enabled = !!fields.enabled.checked;
                slot.name = String(fields.name.value || '').trim();
                slot.host = String(fields.host.value || '').trim();
                slot.port = util.toNumber(fields.port.value, slot.port || 2101);
                slot.mountpoint = String(fields.mountpoint.value || '').trim();
                slot.username = String(fields.username.value || '').trim();

                const password = String(fields.password.value || '');
                if (password) {
                    slot.password = password;
                } else if (card.dataset.hasPassword === '1') {
                    slot.password = '********';
                } else {
                    slot.password = '';
                }
            });

            return slots;
        },

        saveEditor: async function(target, statusTarget) {
            const payload = target && target.__ntripPayload ? target.__ntripPayload : { slots: [] };
            util.setText(statusTarget, 'Saving NTRIP slots...');
            try {
                const slots = this.collectSlots(target, payload);
                await api.post('/api/ntrip', { slots: slots }, { timeoutMs: 8000 });
                util.setText(statusTarget, 'NTRIP slots saved.');
                return true;
            } catch (error) {
                util.setText(statusTarget, 'NTRIP save failed: ' + error.message);
                return false;
            }
        },

        restartRuntime: async function(statusTarget) {
            util.setText(statusTarget, 'Restarting NTRIP runtime...');
            try {
                await api.post('/api/ntrip/restart', undefined, { timeoutMs: 5000 });
                util.setText(statusTarget, 'NTRIP runtime restart requested.');
                return true;
            } catch (error) {
                util.setText(statusTarget, 'NTRIP runtime restart failed: ' + error.message);
                return false;
            }
        },

        renderDashboard: function(summaryTarget, listTarget, payload, capabilities) {
            this.renderRuntimeSummary(summaryTarget, payload, capabilities);
            if (!listTarget) {
                return;
            }
            if (!payload) {
                util.setChildren(listTarget, util.make('div', { class: 'empty-state', text: 'No slot runtime data.' }));
                return;
            }

            const count = visibleCount(payload, capabilities);
            const slots = completeSlots(payload).slice(0, count);
            util.setChildren(listTarget, slots.map(function(slot) {
                return util.make('section', { class: 'slot-runtime-card' }, [
                    util.make('div', { class: 'slot-card-header' }, [
                        util.make('div', {}, [
                            util.make('div', { class: 'font-weight-bold', text: 'Slot ' + slotLetter(slot.index) }),
                            util.make('div', { class: 'small text-muted', text: slot.name || ('Slot ' + slotLetter(slot.index)) })
                        ]),
                        util.badge(slotStatusText(slot), statusTone(slot))
                    ]),
                    factList([
                        { label: 'Enabled', value: slot.enabled ? 'yes' : 'no' },
                        { label: 'Host', value: slot.host || '-' },
                        { label: 'Mountpoint', value: slot.mountpoint || '-' },
                        { label: 'Running', value: slot.running ? 'yes' : 'no' },
                        { label: 'Reconnects', value: slot.reconnect_count != null ? slot.reconnect_count : '-' },
                        { label: 'Drops', value: slot.dropped_rtcm_packets != null ? slot.dropped_rtcm_packets : '-' },
                        { label: 'High-water', value: slot.ringbuffer_high_water != null ? slot.ringbuffer_high_water : '-' },
                        { label: 'Last HTTP code', value: slot.last_http_code || '-' },
                        { label: 'Last error', value: slot.last_error || slot.disabled_reason || '-' }
                    ])
                ]);
            }));
        },

        populateSlotSelect: function(target, payload, capabilities) {
            if (!target) {
                return;
            }
            const count = visibleCount(payload, capabilities);
            const options = [];
            for (let index = 0; index < count; index++) {
                options.push(util.make('option', {
                    value: String(index),
                    text: 'Slot ' + slotLetter(index)
                }));
            }
            util.setChildren(target, options);
        }
    };
})(window);
