(function(global) {
    const app = global.WebUI || global.ConfigPage || {};
    global.WebUI = app;
    global.ConfigPage = app;

    app.gnss = {
        init: function() {
            this.summary = $('.gnss-summary');
            this.diagnosticsSummary = $('.gnss-diagnostics-summary');
            this.constellationSummary = $('.gnss-constellation-summary');
            this.satelliteTable = $('.gnss-satellite-table tbody');
            this.satelliteStatus = $('.gnss-satellite-status');
            this.profileStatus = $('.gnss-profile-status');
            this.profileSelect = $('#gnss-profile');
            this.rawConsole = $('#gnss-raw-console');
            this.baseSummary = $('.gnss-base-summary');
            this.baseStatus = $('.gnss-base-status');
            this.bindEvents();
        },

        renderSummary: function(gnss) {
            if (!this.summary.length || !gnss) return;
            this.summary.empty();

            if (!gnss.detected) {
                this.summary
                    .append($('<div>', { class: 'small font-weight-bold mb-2', text: 'GNSS receiver' }))
                    .append($('<div>', { class: 'small text-muted', text: 'Not connected' }));
                return;
            }

            this.summary
                .append($('<span>', { class: 'capability-pill', text: gnss.receiver_type || 'unknown' }))
                .append($('<span>', { class: 'capability-pill', text: gnss.model || 'unknown model' }))
                .append($('<span>', { class: 'capability-pill', text: gnss.fix_type || 'unknown fix' }))
                .append($('<span>', { class: 'capability-pill', text: (gnss.satellites_used || 0) + ' used / ' + (gnss.satellites_visible || 0) + ' visible' }))
                .append($('<span>', { class: 'capability-pill', text: 'RTK ' + (gnss.rtk_status || 'unknown') }))
                .append($('<span>', { class: 'capability-pill', text: 'CN0 ' + (gnss.cn0_mean || 0) + '/' + (gnss.cn0_max || 0) }))
                .append($('<span>', { class: 'capability-pill', text: 'HDOP ' + (((gnss.hdop_centi || 0) / 100).toFixed(2)) }))
                .append($('<span>', { class: 'capability-pill', text: gnss.rtcm_alive && !gnss.rtcm_stale ? 'RTCM alive' : (gnss.rtcm_stale ? 'RTCM stale' : 'RTCM idle') }));
        },

        renderDiagnostics: function(diagnostics) {
            if (!this.diagnosticsSummary.length || !diagnostics) return;
            this.diagnosticsSummary.empty();

            if (!diagnostics.detected) {
                this.diagnosticsSummary.append($('<div>', { class: 'small text-muted', text: 'No GNSS data' }));
                return;
            }

            this.diagnosticsSummary
                .append($('<span>', { class: 'capability-pill', text: 'Fix ' + (diagnostics.fix_type || 'unknown') }))
                .append($('<span>', { class: 'capability-pill', text: 'Quality ' + (diagnostics.fix_quality || 0) }))
                .append($('<span>', { class: 'capability-pill', text: 'RTK ' + (diagnostics.rtk_status || 'unknown') }))
                .append($('<span>', { class: 'capability-pill', text: 'RTCM ' + (diagnostics.rtcm_state || 'idle') }))
                .append($('<span>', { class: 'capability-pill', text: 'Diff age ' + (diagnostics.diff_age || 0) }))
                .append($('<span>', { class: 'capability-pill', text: 'Base ' + (diagnostics.base_id || '-') }))
                .append($('<span>', { class: 'capability-pill', text: 'Antenna ' + (diagnostics.antenna_status || 'unknown') }))
                .append($('<span>', { class: 'capability-pill', text: 'Jamming ' + (diagnostics.jamming_status || 'unknown') }))
                .append($('<span>', { class: 'capability-pill', text: 'HW ' + (diagnostics.hardware_status || 'unknown') }))
                .append($('<span>', { class: 'capability-pill', text: 'Parser ' + (diagnostics.parser_errors || 0) }))
                .append($('<span>', { class: 'capability-pill', text: 'Last ' + (diagnostics.last_message_ms || 0) + 'ms' }));

            if (typeof diagnostics.agc_main === 'number' && diagnostics.agc_main >= 0) {
                this.diagnosticsSummary.append($('<span>', { class: 'capability-pill', text: 'AGC main ' + diagnostics.agc_main }));
            }
            if (typeof diagnostics.agc_aux === 'number' && diagnostics.agc_aux >= 0) {
                this.diagnosticsSummary.append($('<span>', { class: 'capability-pill', text: 'AGC aux ' + diagnostics.agc_aux }));
            }
        },

        renderSatellites: function(payload) {
            if (!this.satelliteTable.length || !this.satelliteStatus.length) return;
            this.satelliteTable.empty();
            this.satelliteStatus.empty();
            this.constellationSummary.empty();

            if (!payload || !payload.detected) {
                this.satelliteStatus.text('No GNSS data');
                this.constellationSummary.append($('<div>', {
                    class: 'small text-muted',
                    text: 'Connect a receiver to populate satellite diagnostics.'
                }));
                return;
            }

            const satellites = Array.isArray(payload.satellites) ? payload.satellites.slice() : [];
            this.constellationSummary
                .append($('<span>', { class: 'capability-pill', text: (payload.total_visible || 0) + ' visible' }))
                .append($('<span>', { class: 'capability-pill', text: (payload.total_used || 0) + ' used' }))
                .append($('<span>', { class: 'capability-pill', text: 'CN0 ' + (payload.cn0_mean || 0) + '/' + (payload.cn0_max || 0) }))
                .append($('<span>', { class: 'capability-pill', text: 'Fix ' + (payload.fix_type || 'unknown') }))
                .append($('<span>', { class: 'capability-pill', text: 'RTK ' + (payload.rtk_status || 'unknown') }));

            if (Array.isArray(payload.constellations)) {
                payload.constellations.forEach((entry) => {
                    if (!entry || !entry.visible) return;
                    this.constellationSummary.append($('<span>', {
                        class: 'capability-pill',
                        text: entry.name + ' ' + entry.visible + '/' + (entry.used || 0) + ' CN0 ' + (entry.cn0_mean || 0) + '/' + (entry.cn0_max || 0)
                    }));
                });
            }

            if (!satellites.length) {
                this.satelliteStatus.text('Receiver connected, waiting for satellite details');
                return;
            }

            satellites.sort((a, b) => {
                if (!!b.used !== !!a.used) return (b.used ? 1 : 0) - (a.used ? 1 : 0);
                return (b.cn0 || 0) - (a.cn0 || 0);
            });

            satellites.forEach((sat) => {
                this.satelliteTable.append($('<tr>')
                    .append($('<td>', { text: sat.constellation || 'unknown' }))
                    .append($('<td>', { text: sat.prn || sat.svid || 0 }))
                    .append($('<td>', { text: sat.elevation || 0 }))
                    .append($('<td>', { text: sat.azimuth || 0 }))
                    .append($('<td>', { text: sat.cn0 || 0 }))
                    .append($('<td>', { text: sat.signal_id || '-' }))
                    .append($('<td>', { text: sat.used ? 'yes' : 'no' }))
                    .append($('<td>', { text: (sat.last_seen_ms || 0) + 'ms' })));
            });

            this.satelliteStatus.text((payload.total_visible || satellites.length) + ' visible, ' + (payload.total_used || 0) + ' used');
        },

        renderBaseStatus: function(base) {
            if (!this.baseSummary.length || !this.baseStatus.length) return;
            this.baseSummary.empty();
            this.baseStatus.empty();

            const disabled = !base || !base.detected;
            $('#base-start-survey, #base-stop-survey, #base-apply-fixed, #base-clear').prop('disabled', disabled);

            if (!base || !base.detected) {
                this.baseSummary.append($('<div>', { class: 'small text-muted', text: 'No GNSS receiver detected' }));
                this.baseStatus.text(base && base.disabled_reason ? base.disabled_reason : 'Base controls are disabled until a receiver is connected.');
                return;
            }

            this.baseSummary
                .append($('<span>', { class: 'capability-pill', text: 'Mode ' + (base.configured_mode || 'rover') }))
                .append($('<span>', { class: 'capability-pill', text: 'Profile ' + (base.active_profile || 'none') }))
                .append($('<span>', { class: 'capability-pill', text: 'RTCM ' + (base.rtcm_output ? 'on' : 'off') }))
                .append($('<span>', { class: 'capability-pill', text: 'Survey ' + (base.survey_running ? 'running' : 'idle') }))
                .append($('<span>', { class: 'capability-pill', text: 'Target ' + (base.survey_duration_target_s || 0) + 's / ' + (base.survey_accuracy_target_mm || 0) + 'mm' }));

            if (base.has_fixed_position) {
                this.baseSummary
                    .append($('<span>', { class: 'capability-pill', text: 'Lat ' + ((base.latitude_e7 || 0) / 10000000).toFixed(7) }))
                    .append($('<span>', { class: 'capability-pill', text: 'Lon ' + ((base.longitude_e7 || 0) / 10000000).toFixed(7) }))
                    .append($('<span>', { class: 'capability-pill', text: 'Alt ' + ((base.altitude_mm || 0) / 1000).toFixed(3) + 'm' }));
            }

            this.baseStatus.text(base.survey_running
                ? 'Survey running: ' + (base.survey_elapsed_s || 0) + 's elapsed, ' + (base.survey_progress_percent || 0) + '% complete.'
                : (base.last_action_status || 'Idle'));

            $('#base-latitude').val(((base.latitude_e7 || 0) / 10000000).toFixed(7));
            $('#base-longitude').val(((base.longitude_e7 || 0) / 10000000).toFixed(7));
            $('#base-altitude-mm').val(base.altitude_mm || 0);
            $('#base-survey-duration').val(base.survey_duration_target_s || 300);
            $('#base-survey-accuracy').val(base.survey_accuracy_target_mm || 5000);
            $('#base-rtcm-output').prop('checked', !!base.rtcm_output);
        },

        refreshViews: function() {
            const self = this;
            self.refreshDiagnostics();
            self.refreshSatellites();
            self.refreshBaseStatus();
            self.refreshRawConsole();
        },

        refreshDashboardViews: function() {
            this.refreshDiagnostics();
            this.refreshSatellites();
            this.refreshBaseStatus();
        },

        refreshDiagnostics: function() {
            const self = this;
            $.getJSON('api/gnss/diagnostics').done((data) => self.renderDiagnostics(data));
        },

        refreshSatellites: function() {
            const self = this;
            $.getJSON('api/gnss/satellites').done((data) => self.renderSatellites(data));
        },

        refreshBaseStatus: function() {
            const self = this;
            $.getJSON('api/gnss/base/status').done((data) => self.renderBaseStatus(data));
        },

        refreshRawConsole: function() {
            const self = this;
            $.getJSON('api/gnss/receiver/raw').done(function(data) {
                if (!data) return;
                if (self.rawConsole.length) {
                    self.rawConsole.val(data.raw || '');
                    self.rawConsole.scrollTop(self.rawConsole[0].scrollHeight);
                }
                if (self.profileStatus.length) {
                    self.profileStatus.empty()
                        .append($('<span>', { class: 'capability-pill', text: data.profile || 'none' }))
                        .append($('<span>', { class: 'capability-pill', text: data.command_busy ? 'Applying' : 'Idle' }))
                        .append($('<span>', { class: 'capability-pill', text: 'Queue ' + (data.command_queue_depth || 0) }))
                        .append($('<span>', { class: 'capability-pill', text: data.last_command_status || 'idle' }));
                }
            });
        },

        loadProfiles: function() {
            const self = this;
            $.getJSON('api/gnss/profiles').done(function(data) {
                if (!data || !Array.isArray(data.profiles) || !self.profileSelect.length) return;

                const select = self.profileSelect[0];
                if (select) {
                    select.replaceChildren();
                    data.profiles.forEach(function(profile) {
                        const option = document.createElement('option');
                        option.value = profile.name || '';
                        option.textContent = profile.label || profile.name || 'profile';
                        option.selected = !!profile.selected;
                        select.appendChild(option);
                    });
                }

                $('#gnss-baud').val(data.receiver_baud || 115200);
                $('#gnss-nmea-rate').val(data.nmea_rate_hz || 1);
                $('#gnss-rtk-timeout').val(data.rtk_timeout || 60);
                $('#gnss-dgps-timeout').val(data.dgps_timeout || 120);
                $('#gnss-constellation-mask').val(data.constellation_mask || 0);
                $('#gnss-signal-mask').val(data.signal_mask || '');
                $('#gnss-rtcm-output').prop('checked', !!data.rtcm_output);
                $('#gnss-agnss-enable').prop('checked', !!data.agnss_enable);
                $('#base-survey-duration').val(data.base_survey_duration_s || 300);
                $('#base-survey-accuracy').val(data.base_survey_accuracy_mm || 5000);
                $('#base-rtcm-output').prop('checked', !!data.base_rtcm_output);
            });
        },

        bindEvents: function() {
            const self = this;

            $('#gnss-detect').on('click', function() {
                const button = $(this);
                button.prop('disabled', true);
                $.ajax({ url: 'api/gnss/detect', method: 'POST', dataType: 'json' })
                    .done(function(data) {
                        if (data) {
                            self.renderSummary(data);
                            self.refreshViews();
                            self.loadProfiles();
                        }
                    })
                    .always(function() { button.prop('disabled', false); });
            });

            $('#gnss-profile-apply').on('click', function() {
                const button = $(this);
                button.prop('disabled', true);
                $.ajax({
                    url: 'api/gnss/profile/apply',
                    method: 'POST',
                    contentType: 'application/json',
                    data: JSON.stringify({
                        profile: $('#gnss-profile').val(),
                        persist: true,
                        receiver_baud: parseInt($('#gnss-baud').val() || '115200', 10),
                        nmea_rate_hz: parseInt($('#gnss-nmea-rate').val() || '1', 10),
                        rtk_timeout: parseInt($('#gnss-rtk-timeout').val() || '60', 10),
                        dgps_timeout: parseInt($('#gnss-dgps-timeout').val() || '120', 10),
                        constellation_mask: parseInt($('#gnss-constellation-mask').val() || '0', 10),
                        signal_mask: $('#gnss-signal-mask').val(),
                        rtcm_output: $('#gnss-rtcm-output').is(':checked'),
                        agnss_enable: $('#gnss-agnss-enable').is(':checked')
                    })
                }).done(function() {
                    self.refreshViews();
                    self.loadProfiles();
                }).always(function() {
                    button.prop('disabled', false);
                });
            });

            $('#gnss-command-send').on('click', function() {
                const input = $('#gnss-command-input');
                const button = $(this);
                const command = input.val();
                if (!command) return;
                button.prop('disabled', true);
                $.ajax({
                    url: 'api/gnss/command',
                    method: 'POST',
                    contentType: 'application/json',
                    data: JSON.stringify({ command: command })
                }).done(function() {
                    input.val('');
                    self.refreshViews();
                }).always(function() {
                    button.prop('disabled', false);
                });
            });

            $('#base-start-survey').on('click', function() {
                const button = $(this);
                button.prop('disabled', true);
                $.ajax({
                    url: 'api/gnss/base/start-survey',
                    method: 'POST',
                    contentType: 'application/json',
                    data: JSON.stringify({
                        duration_s: parseInt($('#base-survey-duration').val() || '300', 10),
                        accuracy_mm: parseInt($('#base-survey-accuracy').val() || '5000', 10),
                        rtcm_output: $('#base-rtcm-output').is(':checked')
                    })
                }).done(function() {
                    self.refreshViews();
                    self.loadProfiles();
                }).always(function() {
                    button.prop('disabled', false);
                });
            });

            $('#base-stop-survey').on('click', function() {
                const button = $(this);
                button.prop('disabled', true);
                $.post('api/gnss/base/stop-survey').done(function() {
                    self.refreshViews();
                    self.loadProfiles();
                }).always(function() {
                    button.prop('disabled', false);
                });
            });

            $('#base-apply-fixed').on('click', function() {
                const button = $(this);
                button.prop('disabled', true);
                $.ajax({
                    url: 'api/gnss/base/apply-fixed',
                    method: 'POST',
                    contentType: 'application/json',
                    data: JSON.stringify({
                        latitude: parseFloat($('#base-latitude').val() || '0'),
                        longitude: parseFloat($('#base-longitude').val() || '0'),
                        altitude_mm: parseInt($('#base-altitude-mm').val() || '0', 10),
                        rtcm_output: $('#base-rtcm-output').is(':checked')
                    })
                }).done(function() {
                    self.refreshViews();
                    self.loadProfiles();
                }).always(function() {
                    button.prop('disabled', false);
                });
            });

            $('#base-clear').on('click', function() {
                const button = $(this);
                button.prop('disabled', true);
                $.post('api/gnss/base/clear').done(function() {
                    self.refreshViews();
                    self.loadProfiles();
                }).always(function() {
                    button.prop('disabled', false);
                });
            });
        }
    };
})(window);
