(function(global) {
    const app = global.WebUI || global.ConfigPage || {};
    global.WebUI = app;
    global.ConfigPage = app;

    const regionMeta = {
        EU868: { duty_cycle_policy: 'duty_cycle', duty_cycle_window_s: 3600, max_airtime_per_window_ms: 360000 },
        US915: { duty_cycle_policy: 'none', duty_cycle_window_s: 3600, max_airtime_per_window_ms: 0 },
        AU915: { duty_cycle_policy: 'none', duty_cycle_window_s: 3600, max_airtime_per_window_ms: 0 },
        AS923: { duty_cycle_policy: 'lbt_placeholder', duty_cycle_window_s: 3600, max_airtime_per_window_ms: 0 },
        CUSTOM: { duty_cycle_policy: 'custom', duty_cycle_window_s: 0, max_airtime_per_window_ms: 0 }
    };

    function selectField(id, label, options) {
        const select = $('<select>', { class: 'form-control', id: id });
        options.forEach(function(option) {
            select.append($('<option>', { value: option.value, text: option.label }));
        });
        return $('<div>', { class: 'col-md-4' }).append($('<label>', { text: label }), select);
    }

    function inputField(id, label, type, attrs) {
        return $('<div>', { class: 'col-md-4' }).append(
            $('<label>', { text: label }),
            $('<input>', Object.assign({ id: id, type: type, class: 'form-control' }, attrs || {}))
        );
    }

    app.lora = {
        init: function() {
            this.root = $('#lora-panel-root');
            this.renderShell();
            this.bindEvents();
        },

        renderShell: function() {
            const card = $('<div>', { class: 'card mb-3', id: 'lora-panel-card' });
            const body = $('<div>', { class: 'card-body py-3' });
            const info = $('<div>', { class: 'component-note mb-3', text: 'Read-only LoRa build/runtime status. This firmware does not currently expose a WebUI save endpoint for LoRa radio settings.' });
            const summary = $('<div>', { class: 'lora-summary small text-muted mb-3', text: 'Waiting for platform capabilities...' });
            const roleRow = $('<div>', { class: 'form-row' })
                .append(selectField('lora-device-role', 'Device role', [
                    { value: 'base', label: 'base' },
                    { value: 'rover', label: 'rover' },
                    { value: 'dual_debug', label: 'dual_debug' }
                ]))
                .append(selectField('lora-region', 'LoRa region', [
                    { value: 'EU868', label: 'EU868' },
                    { value: 'US915', label: 'US915' },
                    { value: 'AU915', label: 'AU915' },
                    { value: 'AS923', label: 'AS923' },
                    { value: 'CUSTOM', label: 'CUSTOM' }
                ]))
                .append(selectField('lora-chip-family', 'LoRa chip family', [
                    { value: 'SX1261', label: 'SX1261' },
                    { value: 'SX1262', label: 'SX1262' },
                    { value: 'SX1268', label: 'SX1268' },
                    { value: 'LLCC68', label: 'LLCC68' }
                ]));
            const configRow = $('<div>', { class: 'form-row mt-2' })
                .append(inputField('lora-frequency-hz', 'Frequency (Hz)', 'number', { min: '0', step: '1', value: '0' }))
                .append(inputField('lora-tx-power-dbm', 'TX power (dBm)', 'number', { step: '1', value: '14' }))
                .append(selectField('lora-radio-profile', 'Radio profile', [
                    { value: 'rtk_fast', label: 'rtk_fast' },
                    { value: 'custom', label: 'custom' }
                ]));
            const rtcmRow = $('<div>', { class: 'form-row mt-2' })
                .append(selectField('lora-rtcm-profile', 'RTCM profile', [
                    { value: 'rtk_minimal', label: 'rtk_minimal' },
                    { value: 'rtk_gps_only', label: 'rtk_gps_only' },
                    { value: 'rtk_full', label: 'rtk_full' },
                    { value: 'custom', label: 'custom' }
                ]))
                .append($('<div>', { class: 'col-md-8' }).append(
                    $('<label>', { text: 'Resolved regional policy' }),
                    $('<div>', { class: 'lora-readonly-info' })
                        .append($('<span>', { class: 'capability-pill', id: 'lora-duty-cycle-policy', text: 'policy: -' }))
                        .append($('<span>', { class: 'capability-pill', id: 'lora-duty-cycle-window', text: 'window: -' }))
                        .append($('<span>', { class: 'capability-pill', id: 'lora-airtime-budget', text: 'budget: -' }))
                ));
            const validation = $('<div>', { class: 'small text-muted mt-2', id: 'lora-validation-message', text: 'Frequency 0 means auto from the selected region profile.' });

            card.append($('<div>', { class: 'card-header', text: 'LoRa / RTCM Radio' }));
            body.append(info).append(summary).append(roleRow).append(configRow).append(rtcmRow).append(validation);
            body.find('input, select').prop('disabled', true);
            card.append(body);
            this.root.empty().append(card);
        },

        render: function(capabilities) {
            const lora = capabilities && capabilities.lora ? capabilities.lora : {};
            const summary = $('#lora-panel-card .lora-summary');

            if (!capabilities || !capabilities.has_lora_radio) {
                this.root.addClass('panel-hidden');
                return;
            }

            this.root.removeClass('panel-hidden');
            $('#lora-device-role').val((capabilities.device_role || 'base').toLowerCase());
            $('#lora-region').val(lora.region || 'EU868');
            $('#lora-chip-family').val(lora.chip_family || 'SX1262');
            $('#lora-frequency-hz').val(typeof lora.frequency_hz === 'number' ? lora.frequency_hz : 0);
            $('#lora-tx-power-dbm').val(typeof lora.tx_power_dbm === 'number' ? lora.tx_power_dbm : 14);
            $('#lora-radio-profile').val(lora.radio_profile || 'rtk_fast');
            $('#lora-rtcm-profile').val(lora.rtcm_profile || 'rtk_minimal');

            summary.empty()
                .append($('<span>', { class: 'capability-pill', text: 'Role ' + (capabilities.device_role || 'unknown') }))
                .append($('<span>', { class: 'capability-pill', text: 'Region ' + (lora.region || '-') }))
                .append($('<span>', { class: 'capability-pill', text: 'Chip ' + (lora.chip_family || '-') }))
                .append($('<span>', { class: 'capability-pill', text: 'RTCM ' + (lora.rtcm_profile || '-') }))
                .append($('<span>', {
                    class: 'capability-pill',
                    text: lora.tx_enabled ? 'LoRa TX enabled' : 'LoRa TX disabled'
                }));

            this.updatePolicyInfo();
            this.updateValidation();
        },

        renderStatusSummary: function(capabilities, qos) {
            const summary = $('.lora-runtime-summary');
            const lora = capabilities && capabilities.lora ? capabilities.lora : {};

            if (!summary.length) return;
            summary.empty();

            if (!capabilities || !capabilities.has_lora_radio) {
                summary.text('LoRa radio not available on this firmware profile');
                return;
            }

            summary
                .append($('<span>', { class: 'capability-pill', text: 'Role ' + (capabilities.device_role || 'unknown') }))
                .append($('<span>', { class: 'capability-pill', text: 'Region ' + (lora.region || '-') }))
                .append($('<span>', { class: 'capability-pill', text: 'Chip ' + (lora.chip_family || '-') }))
                .append($('<span>', { class: 'capability-pill', text: 'Radio ' + (lora.radio_profile || '-') }))
                .append($('<span>', { class: 'capability-pill', text: 'RTCM ' + (lora.rtcm_profile || '-') }))
                .append($('<span>', { class: 'capability-pill', text: lora.tx_enabled ? 'LoRa TX enabled' : 'LoRa TX disabled' }))
                .append($('<span>', { class: 'capability-pill', text: (lora.frequency_hz || 0) > 0 ? (lora.frequency_hz + ' Hz') : 'Auto frequency' }))
                .append($('<span>', { class: 'capability-pill', text: (lora.tx_power_dbm || 0) + ' dBm' }));

            if (lora.duty_cycle_policy) {
                summary.append($('<span>', { class: 'capability-pill', text: 'Policy ' + lora.duty_cycle_policy }));
            }

            if (qos && qos.state) {
                summary.append($('<span>', { class: 'capability-pill', text: 'QoS ' + qos.state }));
            }
        },

        updatePolicyInfo: function() {
            const region = $('#lora-region').val() || 'EU868';
            const meta = regionMeta[region] || regionMeta.EU868;
            $('#lora-duty-cycle-policy').text('policy: ' + meta.duty_cycle_policy);
            $('#lora-duty-cycle-window').text('window: ' + (meta.duty_cycle_window_s ? meta.duty_cycle_window_s + 's' : 'explicit'));
            $('#lora-airtime-budget').text('budget: ' + (meta.max_airtime_per_window_ms ? meta.max_airtime_per_window_ms + 'ms' : 'explicit'));
        },

        updateValidation: function() {
            const region = $('#lora-region').val();
            const frequency = parseInt($('#lora-frequency-hz').val() || '0', 10);
            const message = $('#lora-validation-message');
            const invalid = region === 'CUSTOM' && frequency === 0;

            $('#lora-frequency-hz').toggleClass('is-invalid', invalid);
            if (invalid) {
                message.text('CUSTOM region requires an explicit non-zero frequency.');
            } else if (frequency === 0) {
                message.text('Frequency 0 means auto from the selected region profile.');
            } else {
                message.text('Frequency is explicitly set for this preview.');
            }
        },

        bindEvents: function() {
            const self = this;
            this.root.on('change input', '#lora-region, #lora-frequency-hz', function() {
                self.updatePolicyInfo();
                self.updateValidation();
            });
        }
    };
})(window);
