(function(global) {
    const WebUI = global.WebUI = global.WebUI || {};
    const util = WebUI.util;

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
            : util.make('div', { class: 'empty-state', text: 'No LoRa data available.' });
    }

    WebUI.lora = {
        renderReadOnly: function(target, capabilities, options) {
            if (!target) {
                return;
            }

            const context = options && options.context ? options.context : 'dashboard';
            if (!capabilities) {
                util.setChildren(target, util.make('div', { class: 'empty-state', text: 'LoRa status unavailable.' }));
                return;
            }

            if (!capabilities.has_lora_radio) {
                util.setChildren(target, [
                    util.make('div', { class: 'notice notice-warning', text: 'LoRa unavailable for this firmware profile.' }),
                    factList([
                        { label: 'Driver', value: capabilities.lora_driver || 'none' },
                        { label: 'TX enabled', value: capabilities.lora_tx_enabled ? 'yes' : 'no' }
                    ])
                ]);
                return;
            }

            const notices = [];
            if (!capabilities.lora_ready) {
                notices.push(util.make('div', {
                    class: 'notice notice-warning',
                    text: 'LoRa profile is compiled in, but the radio is not ready at runtime.'
                }));
            }
            if (!capabilities.lora_tx_enabled) {
                notices.push(util.make('div', {
                    class: 'notice notice-info',
                    text: 'RTK LoRa TX is disabled by default in this firmware.'
                }));
            }
            if (context === 'config') {
                notices.push(util.make('div', {
                    class: 'notice',
                    text: 'LoRa is read-only in WebUI because no firmware save endpoint is implemented.'
                }));
            }

            util.setChildren(target, notices.concat([
                factList([
                    { label: 'Available in build', value: capabilities.has_lora_radio ? 'yes' : 'no' },
                    { label: 'Runtime ready', value: capabilities.lora_ready ? 'yes' : 'no', tone: capabilities.lora_ready ? 'success' : 'warning' },
                    { label: 'Chip', value: capabilities.lora && capabilities.lora.chip_family ? capabilities.lora.chip_family : '-' },
                    { label: 'Driver', value: capabilities.lora_driver || (capabilities.lora && capabilities.lora.driver) || 'none' },
                    { label: 'Region', value: capabilities.lora && capabilities.lora.region ? capabilities.lora.region : '-' },
                    { label: 'Frequency', value: util.formatFrequency(capabilities.lora && capabilities.lora.frequency_hz) },
                    { label: 'Radio profile', value: capabilities.lora && capabilities.lora.radio_profile ? capabilities.lora.radio_profile : '-' },
                    { label: 'RTCM profile', value: capabilities.lora && capabilities.lora.rtcm_profile ? capabilities.lora.rtcm_profile : '-' },
                    { label: 'Duty cycle policy', value: capabilities.lora && capabilities.lora.duty_cycle_policy ? capabilities.lora.duty_cycle_policy : '-' },
                    { label: 'TX enabled', value: capabilities.lora_tx_enabled ? 'yes' : 'no', tone: capabilities.lora_tx_enabled ? 'success' : 'warning' }
                ])
            ]));
        }
    };
})(window);
