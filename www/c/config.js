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
    app.utils.printEscape = function(str) {
        return String(str || '')
            .replace(/\\/g, '\\\\')
            .replace(/\n/g, '\\n')
            .replace(/\r/g, '\\r')
            .replace(/\t/g, '\\t');
    };

    app.utils.printUnescape = function(str) {
        return String(str || '')
            .replace(/\\\\/g, '\\')
            .replace(/\\n/g, '\n')
            .replace(/\\r/g, '\r')
            .replace(/\\t/g, '\t');
    };

    app.utils.secondsToHHMMSS = function(seconds) {
        let value = parseInt(seconds || 0, 10);
        const hours = Math.floor(value / 3600);
        const minutes = Math.floor(value / 60) % 60;
        value %= 60;
        return [hours, minutes, value].map((entry) => entry < 10 ? '0' + entry : '' + entry).join(':');
    };

    app.utils.secondsToShort = function(seconds) {
        let value = parseInt(seconds || 0, 10);
        const hours = Math.floor(value / 3600);
        const minutes = Math.floor((value % 3600) / 60);
        value %= 60;
        return [hours, minutes, value].map((entry) => entry < 10 ? '0' + entry : '' + entry).join(':');
    };

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

    app.utils.pollDelayForQos = function(state, normal, degraded, critical) {
        if (state === 'critical') return critical;
        if (state === 'degraded') return degraded;
        return normal;
    };

    app.utils.wifiRssiColorClass = function(rssi) {
        if (rssi > -50) return 'primary';
        if (rssi > -60) return 'success';
        if (rssi > -70) return 'warning';
        return 'danger';
    };

    app.utils.appendText = function(target, text) {
        $(target).append(document.createTextNode(String(text)));
        return $(target);
    };

    $.fn.appendText = function(text) {
        return app.utils.appendText(this, text);
    };

    app.autoTab = function(target) {
        const length = target.value.length;
        if (length >= target.maxLength) {
            target.value = target.value.slice(-1);
            let next = target;
            while ((next = next.nextElementSibling)) {
                if (next.tagName && next.tagName.toLowerCase() === 'input') {
                    next.focus();
                    next.select();
                    break;
                }
            }
        } else if (length === 0) {
            let previous = target;
            while ((previous = previous.previousElementSibling)) {
                if (previous.tagName && previous.tagName.toLowerCase() === 'input') {
                    previous.focus();
                    break;
                }
            }
        }
    };

    app.initForm = function() {
        const form = $('#form');
        const projectVersionText = $('#project-version');

        app.form = form;

        $('[data-toggle="tooltip"]').tooltip({
            animation: false,
            container: 'body',
            html: true
        });

        form.on('submit', function(event) {
            event.preventDefault();
            event.stopPropagation();

            if (form[0].checkValidity() !== false) {
                const submit = form.find(':submit').prop('disabled', true);
                const data = JSON.stringify(form.serializeObject());

                $.post('/config', data).done(function() {
                    $('#restarting-modal').modal('show');
                    setTimeout(function() {
                        app.state.reloadOnStatus = true;
                    }, 2500);
                }).always(function() {
                    submit.prop('disabled', false);
                });
            }

            form.addClass('was-validated');
        });

        form.find("input[type='checkbox']").each(function() {
            const checkbox = $(this);
            const hidden = $('<input>')
                .attr('name', checkbox.attr('name'))
                .attr('type', 'hidden')
                .data('checkbox', true)
                .attr('value', '0')
                .prop('disabled', checkbox.prop('checked'))
                .insertAfter(checkbox);

            checkbox.on('change', function() {
                hidden.prop('disabled', checkbox.prop('checked'));
            });
        });

        app.checkDisableIfs = function() {
            form.find('[data-disable-if]').each(function() {
                const target = $(this);
                let condition = target.data('disable-if-condition');
                if (typeof condition === 'undefined') condition = ':not(:checked)';

                const disabled = form.find(target.data('disable-if')).filter(condition).length > 0;
                const current = target.hasClass('disabled');
                if (disabled === current) return;

                target.prop('disabled', disabled);
                target.toggleClass('disabled', disabled);

                if (target.is('div')) {
                    target.find(':input').each(function() {
                        const input = $(this);
                        if (disabled) {
                            input.data('prop-disabled-previous', input.prop('disabled') === true)
                                .data('class-disabled-previous', input.hasClass('disabled'))
                                .prop('disabled', true)
                                .addClass('disabled');
                        } else {
                            input.prop('disabled', input.data('prop-disabled-previous') === true)
                                .toggleClass('disabled', input.data('class-disabled-previous') === true);
                        }
                    });
                }
            });

            app.hideLegacyConfigCards();
        };

        app.checkDisableIfs();
        form.find(':input').on('change', app.checkDisableIfs);

        $('#switch-uart').on('click', function() {
            if (confirm('WARNING\n\nThese settings are mainly for development purposes and do not normally have to be changed. With a standard ESP32 RTK Gateway device the only setting you may want to change is the UART baud rate. DO NOT PROCEED unless you know what you are doing.')) {
                $(this).hide();
                app.checkDisableIfs();
            }
        });

        if (projectVersionText.length) {
            $.getJSON('/config').done(function(data) {
                projectVersionText.text(data.version);
                projectVersionText.attr('href', 'https://github.com/Pepeuch/esp32-rtk-gateway')
                    .removeClass('text-muted')
                    .addClass('text-primary');

                app.loadConfig(data);
            });
        }

        app.bindCommonInputs();
        return app;
    };

    app.hideLegacyConfigCards = function() {
        $('.legacy-config-card').each(function() {
            const card = $(this);
            card.addClass('panel-hidden');
            card.find(':input').each(function() {
                $(this).prop('disabled', true).addClass('disabled');
            });
        });
    };

    app.loadConfig = function(data) {
        if (!data || !app.form) return;

        app.form.find(':input').each(function() {
            let value;
            const input = $(this);

            if (!this.name || typeof data[this.name] === 'undefined') return;

            value = data[this.name];
            if (Array.isArray(value)) {
                const index = app.form.find(':input[name="' + this.name + '"]').index(this);
                if (value.length <= index) return;
                value = value[index];
            }

            if (this.type === 'checkbox' || this.type === 'radio') {
                const active = input.val() == value;
                input.prop('checked', active);
                input.parent('.btn').toggleClass('active', active);
            } else if (this.type === 'hidden' && input.data('checkbox')) {
                input.prop('disabled', input.val() != value);
            } else {
                input.val(value);
            }

            if (input.data('formatted')) input.trigger('change');
        });

        app.checkDisableIfs();
        app.hideLegacyConfigCards();
    };

    app.bindCommonInputs = function() {
        const form = app.form;
        const wifiNetworksScanButton = form.find('.wifi-networks-scan');
        const wifiNetworksDropdownButton = form.find('.wifi-networks-dropdown');
        const wifiNetworksList = form.find('.wifi-networks');
        const wifiNetworksOpenList = wifiNetworksList.find('.wifi-networks-open');
        const wifiNetworksSecuredList = wifiNetworksList.find('.wifi-networks-secured');
        const wifiNetworksSsidInput = form.find('.wifi-networks-ssid');
        const wifiNetworksPasswordInput = form.find('.wifi-networks-pass');
        const wifiNetworksPasswordEmptyInput = form.find('.wifi-networks-pass-empty');
        const wifiApGwInputs = form.find('input[name="w_ap_gw"]');
        const wifiApGwPrefixText = form.find('.wifi-ap-ip-prefix');
        const wifiApIpMinInput = form.find('input[name="w_ap_ip_min"]');
        const wifiApIpMaxInput = form.find('input[name="w_ap_ip_max"]');
        const wifiApSubnetInput = form.find('select[name="w_ap_subnet"]');
        const socketClientConnectMessageInput = form.find('input[name="sck_cli_msg"]');
        const socketClientConnectMessageUnformattedInput = form.find('input[name="sck_cli_msg_unformatted"]');

        wifiNetworksScanButton.on('click', function() {
            wifiNetworksOpenList.empty();
            wifiNetworksSecuredList.empty();
            wifiNetworksScanButton.prop('disabled', true);
            wifiNetworksDropdownButton.prop('disabled', true);

            $.getJSON('/wifi/scan').always(function() {
                wifiNetworksScanButton.prop('disabled', false);
                wifiNetworksDropdownButton.prop('disabled', false);
            }).done(function(networks) {
                networks.forEach(function(network) {
                    const open = network.authmode === 'OPEN';
                    const entry = $('<a>', {
                        class: 'wifi-network dropdown-item',
                        href: 'javascript:void(0);'
                    });

                    entry.data('ssid', network.ssid);
                    entry.data('rssi', network.rssi);
                    entry.data('authmode', network.authmode);
                    entry.data('open', open);
                    entry.appendText(network.ssid + (open ? '' : ' (' + network.authmode + ') '))
                        .append($('<span>', {
                            class: 'text-' + app.utils.wifiRssiColorClass(network.rssi),
                            text: network.rssi + 'dBm'
                        }));

                    entry.appendTo(open ? wifiNetworksOpenList : wifiNetworksSecuredList);
                });

                wifiNetworksList.find('.wifi-network').on('click', function() {
                    wifiNetworksSsidInput.val($(this).data('ssid'));
                    wifiNetworksPasswordInput.val('').prop('disabled', $(this).data('open'));
                    wifiNetworksPasswordEmptyInput.val('').prop('disabled', !$(this).data('open'));
                });

                wifiNetworksDropdownButton.trigger('click');
            });
        });

        wifiNetworksSsidInput.on('input change', function() {
            wifiNetworksPasswordInput.prop('disabled', false);
            wifiNetworksPasswordEmptyInput.prop('disabled', true);
        });

        wifiApSubnetInput.add(wifiApGwInputs).on('change input', function() {
            const parts = wifiApGwInputs.map((_, el) => Math.max(0, Math.min(255, el.value))).get();
            const subnet = parseInt(wifiApSubnetInput.children('option:selected').val(), 10);
            const mask = 255 >> (subnet - 24);
            const gateway = parts[3];
            const min = (gateway & ~mask) + 1;
            const max = (gateway | mask) - 1;

            wifiApGwPrefixText.text(parts.slice(0, 3).join('.') + '.');
            wifiApGwInputs.eq(3).prop('min', min).prop('max', max).toggleClass('is-invalid', gateway < min || gateway > max);
            wifiApIpMinInput.val(min);
            wifiApIpMaxInput.val(max);
        }).trigger('change');

        socketClientConnectMessageInput.on('change', function() {
            socketClientConnectMessageUnformattedInput.val(app.utils.printEscape($(this).val()));
        });
        socketClientConnectMessageUnformattedInput.on('change', function() {
            socketClientConnectMessageInput.val(app.utils.printUnescape($(this).val()));
        });
    };

    app.applyRoleVisibility = function(capabilitiesOverride) {
        const capabilities = capabilitiesOverride || app.state.capabilities || {};
        const role = String(capabilities.device_role || 'base').toLowerCase();
        const showBaseOnly = role === 'base' || role === 'dual_debug';
        const hasLora = !!capabilities.has_lora_radio;

        $('.base-role-card').toggleClass('panel-hidden', !showBaseOnly);
        $('#lora-panel-root').toggleClass('panel-hidden', !hasLora);
        app.hideLegacyConfigCards();
    };
})(window);
