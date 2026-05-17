(function(global) {
    const WebUI = global.WebUI = global.WebUI || {};
    const util = WebUI.util;
    const api = WebUI.api;

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
            : util.make('div', { class: 'empty-state', text: 'No GNSS data available.' });
    }

    function pillRow(values) {
        return util.make('div', { class: 'pill-list' }, values.filter(Boolean).map(function(value) {
            return util.pill(value);
        }));
    }

    function latitudeFromE7(value) {
        return (Number(value || 0) / 10000000).toFixed(7);
    }

    function longitudeFromE7(value) {
        return (Number(value || 0) / 10000000).toFixed(7);
    }

    function accuracyText(value) {
        const numeric = Number(value || 0);
        return numeric ? (numeric + ' mm') : '-';
    }

    function setInputValue(selector, value) {
        const element = util.q(selector);
        if (element && value != null) {
            element.value = value;
        }
    }

    WebUI.gnss = {
        loadStatus: function() {
            return api.get('/api/gnss/status');
        },

        loadProfiles: function() {
            return api.get('/api/gnss/profiles');
        },

        loadDiagnostics: function() {
            return api.get('/api/gnss/diagnostics');
        },

        loadSatellites: function() {
            return api.get('/api/gnss/satellites');
        },

        loadBaseStatus: function() {
            return api.get('/api/gnss/base/status');
        },

        loadRawConsole: function() {
            return api.get('/api/gnss/receiver/raw', { timeoutMs: 5000 });
        },

        detect: function() {
            return api.post('/api/gnss/detect', {});
        },

        applyProfile: function(payload) {
            return api.post('/api/gnss/profile/apply', payload, { timeoutMs: 8000 });
        },

        startSurvey: function(payload) {
            return api.post('/api/gnss/base/start-survey', payload, { timeoutMs: 8000 });
        },

        stopSurvey: function() {
            return api.post('/api/gnss/base/stop-survey', undefined, { timeoutMs: 8000 });
        },

        applyFixed: function(payload) {
            return api.post('/api/gnss/base/apply-fixed', payload, { timeoutMs: 8000 });
        },

        clearBase: function() {
            return api.post('/api/gnss/base/clear', undefined, { timeoutMs: 8000 });
        },

        renderOverview: function(target, gnss) {
            if (!target) {
                return;
            }
            if (!gnss || !gnss.success) {
                util.setChildren(target, util.make('div', { class: 'empty-state', text: 'GNSS status unavailable.' }));
                return;
            }
            if (!gnss.detected) {
                util.setChildren(target, util.make('div', { class: 'empty-state', text: 'No GNSS receiver detected.' }));
                return;
            }

            util.setChildren(target, [
                pillRow([
                    gnss.receiver_type || 'unknown receiver',
                    gnss.profile ? ('Profile ' + gnss.profile) : 'Profile none',
                    gnss.fix_type || 'fix unknown',
                    gnss.rtk_status ? ('RTK ' + gnss.rtk_status) : 'RTK unknown',
                    String(gnss.satellites_used || 0) + '/' + String(gnss.satellites_visible || 0) + ' sats'
                ]),
                factList([
                    { label: 'Receiver type', value: gnss.receiver_type || '-' },
                    { label: 'Model', value: gnss.model || '-' },
                    { label: 'Firmware', value: gnss.firmware || '-' },
                    { label: 'Profile', value: gnss.profile || '-' },
                    { label: 'Fix type', value: gnss.fix_type || '-' },
                    { label: 'Carrier / RTK', value: gnss.rtk_status || '-' },
                    { label: 'Satellites', value: String(gnss.satellites_used || 0) + ' used / ' + String(gnss.satellites_visible || 0) + ' visible' },
                    { label: 'Parser errors', value: gnss.parser_errors != null ? gnss.parser_errors : '-' },
                    { label: 'Last command status', value: gnss.last_command_status || '-' }
                ])
            ]);
        },

        renderProfileStatus: function(target, profileState, gnssStatus) {
            if (!target) {
                return;
            }
            const profile = (profileState && profileState.selected_profile) || (gnssStatus && gnssStatus.profile) || 'none';
            const queueDepth = gnssStatus && gnssStatus.command_queue_depth != null ? gnssStatus.command_queue_depth : 0;
            const pending = !!(gnssStatus && (gnssStatus.profile_pending || gnssStatus.command_busy));
            const lastCommandStatus = (gnssStatus && gnssStatus.last_command_status) || 'idle';

            util.setChildren(target, [
                pillRow([
                    profile ? ('Profile ' + profile) : 'Profile none',
                    pending ? 'Apply pending' : 'Idle',
                    'Queue ' + queueDepth,
                    lastCommandStatus
                ])
            ]);
        },

        renderDiagnostics: function(target, diagnostics) {
            if (!target) {
                return;
            }
            if (!diagnostics || !diagnostics.success) {
                util.setChildren(target, util.make('div', { class: 'empty-state', text: 'GNSS diagnostics unavailable.' }));
                return;
            }

            util.setChildren(target, [
                pillRow([
                    diagnostics.antenna_status ? ('Antenna ' + diagnostics.antenna_status) : null,
                    diagnostics.jamming_status ? ('Jamming ' + diagnostics.jamming_status) : null,
                    diagnostics.rtcm_state ? ('RTCM ' + diagnostics.rtcm_state) : null,
                    diagnostics.fix_type ? ('Fix ' + diagnostics.fix_type) : null
                ]),
                factList([
                    { label: 'Antenna', value: diagnostics.antenna_status || '-' },
                    { label: 'AGC main', value: diagnostics.agc_main != null ? diagnostics.agc_main : '-' },
                    { label: 'AGC aux', value: diagnostics.agc_aux != null ? diagnostics.agc_aux : '-' },
                    { label: 'Jamming', value: diagnostics.jamming_status || '-' },
                    { label: 'Parser errors', value: diagnostics.parser_errors != null ? diagnostics.parser_errors : '-' },
                    { label: 'Last command status', value: diagnostics.last_command_status || '-' },
                    { label: 'Horizontal accuracy', value: accuracyText(diagnostics.horizontal_accuracy_mm) },
                    { label: 'Vertical accuracy', value: accuracyText(diagnostics.vertical_accuracy_mm) },
                    { label: 'Diff age', value: diagnostics.diff_age != null ? diagnostics.diff_age : '-' },
                    { label: 'Base ID', value: diagnostics.base_id || '-' }
                ])
            ]);
        },

        renderSatellites: function(targets, satellites) {
            if (!targets || !targets.summary || !targets.tableBody || !targets.status) {
                return;
            }
            if (!satellites || !satellites.success) {
                util.setChildren(targets.summary, util.make('div', { class: 'empty-state', text: 'Satellite data unavailable.' }));
                util.setChildren(targets.tableBody, []);
                if (targets.tableWrap) {
                    util.show(targets.tableWrap, false);
                }
                util.setText(targets.status, 'No satellite data.');
                return;
            }

            util.setChildren(targets.summary, [
                pillRow((satellites.constellations || []).map(function(entry) {
                    return entry.name + ' ' + entry.used + '/' + entry.visible;
                }))
            ]);

            const rows = (satellites.satellites || []).map(function(entry) {
                return util.make('tr', {}, [
                    util.make('td', { text: entry.constellation || '-' }),
                    util.make('td', { text: entry.prn != null ? entry.prn : '-' }),
                    util.make('td', { text: entry.elevation != null ? entry.elevation : '-' }),
                    util.make('td', { text: entry.azimuth != null ? entry.azimuth : '-' }),
                    util.make('td', { text: entry.cn0 != null ? entry.cn0 : '-' }),
                    util.make('td', { text: entry.signal_id != null ? entry.signal_id : '-' }),
                    util.make('td', { text: entry.used ? 'yes' : 'no' }),
                    util.make('td', { text: entry.last_seen_ms != null ? entry.last_seen_ms : '-' })
                ]);
            });

            util.setChildren(targets.tableBody, rows);
            if (targets.tableWrap) {
                util.show(targets.tableWrap, rows.length > 0);
            }
            util.setText(
                targets.status,
                rows.length
                    ? ('Showing ' + rows.length + ' satellites, ' + String(satellites.total_used || 0) + ' used.')
                    : 'No satellite table rows available.'
            );
        },

        renderBaseStatus: function(target, status) {
            if (!target) {
                return;
            }
            if (!status || !status.success) {
                util.setChildren(target, util.make('div', { class: 'empty-state', text: 'Base station status unavailable.' }));
                return;
            }

            util.setChildren(target, [
                pillRow([
                    status.configured_mode ? ('Mode ' + status.configured_mode) : null,
                    status.active_profile ? ('Profile ' + status.active_profile) : null,
                    status.survey_running ? 'Survey running' : (status.has_fixed_position ? 'Fixed position' : 'No fixed position')
                ]),
                factList([
                    { label: 'Configured mode', value: status.configured_mode || '-' },
                    { label: 'Active profile', value: status.active_profile || '-' },
                    { label: 'Receiver mode', value: status.receiver_mode || '-' },
                    { label: 'Survey running', value: status.survey_running ? 'yes' : 'no' },
                    { label: 'Fixed position', value: status.has_fixed_position ? 'yes' : 'no' },
                    { label: 'Latitude', value: latitudeFromE7(status.latitude_e7) },
                    { label: 'Longitude', value: longitudeFromE7(status.longitude_e7) },
                    { label: 'Altitude', value: String(status.altitude_mm || 0) + ' mm' },
                    { label: 'Survey progress', value: String(status.survey_progress_percent || 0) + '%' },
                    { label: 'Last action', value: status.last_action_status || '-' },
                    { label: 'Disabled reason', value: status.disabled_reason || '-' }
                ])
            ]);
        },

        populateProfileForm: function(root, profilesPayload, gnssStatus) {
            if (!root || !profilesPayload) {
                return;
            }

            const profileSelect = util.q('#gnss-profile', root);
            if (profileSelect) {
                const options = (profilesPayload.profiles || []).map(function(profile) {
                    return util.make('option', {
                        value: profile.name,
                        text: profile.label || profile.name
                    });
                });
                util.setChildren(profileSelect, options);
                profileSelect.value = profilesPayload.selected_profile || (gnssStatus && gnssStatus.profile) || '';
            }

            setInputValue('#gnss-baud', profilesPayload.receiver_baud != null ? profilesPayload.receiver_baud : '');
            setInputValue('#gnss-nmea-rate', profilesPayload.nmea_rate_hz != null ? profilesPayload.nmea_rate_hz : '');
            setInputValue('#gnss-rtk-timeout', profilesPayload.rtk_timeout != null ? profilesPayload.rtk_timeout : '');
            setInputValue('#gnss-dgps-timeout', profilesPayload.dgps_timeout != null ? profilesPayload.dgps_timeout : '');
            setInputValue('#gnss-constellation-mask', profilesPayload.constellation_mask != null ? profilesPayload.constellation_mask : '');
            setInputValue('#gnss-signal-mask', profilesPayload.signal_mask != null ? profilesPayload.signal_mask : '');

            const rtcmOutput = util.q('#gnss-rtcm-output', root);
            if (rtcmOutput) rtcmOutput.checked = !!profilesPayload.rtcm_output;
            const agnssEnable = util.q('#gnss-agnss-enable', root);
            if (agnssEnable) agnssEnable.checked = !!profilesPayload.agnss_enable;
        },

        populateBaseForm: function(root, profilesPayload, baseStatus) {
            if (!root) {
                return;
            }

            const latitudeE7 = baseStatus && baseStatus.latitude_e7 != null ? baseStatus.latitude_e7 : profilesPayload.base_latitude_e7;
            const longitudeE7 = baseStatus && baseStatus.longitude_e7 != null ? baseStatus.longitude_e7 : profilesPayload.base_longitude_e7;
            const altitudeMm = baseStatus && baseStatus.altitude_mm != null ? baseStatus.altitude_mm : profilesPayload.base_altitude_mm;
            const surveyDuration = baseStatus && baseStatus.survey_duration_target_s != null ? baseStatus.survey_duration_target_s : profilesPayload.base_survey_duration_s;
            const surveyAccuracy = baseStatus && baseStatus.survey_accuracy_target_mm != null ? baseStatus.survey_accuracy_target_mm : profilesPayload.base_survey_accuracy_mm;
            const rtcmOutput = baseStatus && typeof baseStatus.rtcm_output === 'boolean' ? baseStatus.rtcm_output : profilesPayload.base_rtcm_output;

            setInputValue('#base-latitude', latitudeFromE7(latitudeE7 || 0));
            setInputValue('#base-longitude', longitudeFromE7(longitudeE7 || 0));
            setInputValue('#base-altitude-mm', altitudeMm != null ? altitudeMm : '');
            setInputValue('#base-survey-duration', surveyDuration != null ? surveyDuration : '');
            setInputValue('#base-survey-accuracy', surveyAccuracy != null ? surveyAccuracy : '');
            const checkbox = util.q('#base-rtcm-output', root);
            if (checkbox) checkbox.checked = !!rtcmOutput;
            const modeNote = util.q('#base-mode-note', root);
            if (modeNote) {
                util.setText(modeNote, 'Current mode: ' + ((baseStatus && baseStatus.configured_mode) || profilesPayload.base_mode || 'disabled'));
            }
        },

        renderRawConsole: function(target, data) {
            if (!target) {
                return;
            }
            if (!data || !data.success) {
                target.value = 'Raw console unavailable.';
                return;
            }
            target.value = data.raw || '';
        }
    };
})(window);
