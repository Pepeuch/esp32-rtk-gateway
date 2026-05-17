(function(global) {
    const WebUI = global.WebUI = global.WebUI || {};
    const util = WebUI.util;
    const api = WebUI.api;

    const LOG_PAGE_INDEX_KEY = 'webui_log_page_index';
    const MAX_LOG_ROWS = 500;
    const LOG_PATTERN = /(?<level>[VDIWER]) \((?<time>[\d:.]+)\) (?<tag>[a-zA-Z0-9_-]+): (?<comment>.*)/;

    function levelMeta(level) {
        switch (level) {
            case 'R': return { label: 'RESET', tone: 'info' };
            case 'V': return { label: 'VERBOSE', tone: 'light' };
            case 'D': return { label: 'DEBUG', tone: 'secondary' };
            case 'I': return { label: 'INFO', tone: 'success' };
            case 'W': return { label: 'WARN', tone: 'warning' };
            case 'E': return { label: 'ERROR', tone: 'danger' };
            default: return { label: level || '?', tone: '' };
        }
    }

    const logPage = {
        state: {
            running: false,
            pageIndex: 0,
            rowIndex: 1
        },

        init: function() {
            this.refs = {
                status: util.q('#log-status'),
                tbody: util.q('#log-rows'),
                reloadNotice: util.q('#log-reload-notice'),
                reconnect: util.q('#log-reconnect'),
                clear: util.q('#log-clear')
            };

            this.state.pageIndex = Number(localStorage.getItem(LOG_PAGE_INDEX_KEY) || '0') + 1;
            localStorage.setItem(LOG_PAGE_INDEX_KEY, String(this.state.pageIndex));
            this.state.running = true;

            this.refs.reconnect.addEventListener('click', this.refresh.bind(this));
            this.refs.clear.addEventListener('click', this.clear.bind(this));

            this.pollLoop();
        },

        clear: function() {
            this.state.rowIndex = 1;
            util.setChildren(this.refs.tbody, []);
            util.setText(this.refs.status, 'Log cleared. Waiting for new lines...');
        },

        pollLoop: async function() {
            while (this.state.running) {
                await this.refresh();
                await new Promise(function(resolve) { global.setTimeout(resolve, 2500); });
            }
        },

        refresh: async function() {
            if (Number(localStorage.getItem(LOG_PAGE_INDEX_KEY) || '0') > this.state.pageIndex) {
                util.show(this.refs.reloadNotice, true);
                util.setText(this.refs.status, 'Another log page is active. Reload or use the newest tab.');
                return;
            }

            util.show(this.refs.reloadNotice, false);

            try {
                const text = await api.get('/log', { timeoutMs: 3000 });
                this.appendLines(String(text || '').split('\n'));
                util.setText(this.refs.status, 'Connected. Polling /log every 2.5 s.');
            } catch (error) {
                util.setText(this.refs.status, 'Log fetch failed: ' + error.message);
            }
        },

        appendLines: function(lines) {
            const rows = [];

            lines.forEach(function(line) {
                if (!line) {
                    return;
                }

                let normalized = line;
                if (line === '@@@@') {
                    normalized = 'R (00:00:00.000) ESP32: Device Restarted';
                }

                const match = normalized.match(LOG_PATTERN);
                if (!match || !match.groups) {
                    return;
                }

                const meta = levelMeta(match.groups.level);
                const row = util.make('tr', { class: match.groups.level === 'R' ? 'table-info' : '' }, [
                    util.make('td', { text: String(logPage.state.rowIndex++) }),
                    util.make('td', { text: match.groups.time + 'ms' }),
                    util.make('td', {}, util.badge(meta.label, meta.tone)),
                    util.make('td', { text: match.groups.tag }),
                    util.make('td', { text: match.groups.comment })
                ]);
                rows.push(row);
            });

            rows.forEach(function(row) {
                logPage.refs.tbody.appendChild(row);
            });

            while (logPage.refs.tbody.children.length > MAX_LOG_ROWS) {
                logPage.refs.tbody.removeChild(logPage.refs.tbody.firstElementChild);
            }
        }
    };

    WebUI.logPage = logPage;
})(window);
