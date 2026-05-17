(function(global) {
    const WebUI = global.WebUI = global.WebUI || {};

    const SECRET_UNCHANGED = '\x1a\x1a\x1a\x1a\x1a\x1a\x1a\x1a';
    const DEFAULT_TIMEOUT_MS = 4000;

    function appendChild(parent, child) {
        if (child == null) {
            return;
        }
        if (Array.isArray(child)) {
            child.forEach(function(entry) {
                appendChild(parent, entry);
            });
            return;
        }
        if (child instanceof Node) {
            parent.appendChild(child);
            return;
        }
        parent.appendChild(document.createTextNode(String(child)));
    }

    function make(tagName, props, children) {
        const element = document.createElement(tagName);
        const options = props || {};

        Object.keys(options).forEach(function(key) {
            const value = options[key];
            if (value == null) {
                return;
            }
            if (key === 'class') {
                element.className = value;
            } else if (key === 'text') {
                element.textContent = value;
            } else if (key === 'html') {
                element.innerHTML = value;
            } else if (key === 'value') {
                element.value = value;
            } else if (key === 'checked') {
                element.checked = !!value;
            } else if (key === 'dataset') {
                Object.keys(value).forEach(function(name) {
                    element.dataset[name] = value[name];
                });
            } else if (key === 'attrs') {
                Object.keys(value).forEach(function(name) {
                    if (value[name] != null) {
                        element.setAttribute(name, String(value[name]));
                    }
                });
            } else {
                element[key] = value;
            }
        });

        appendChild(element, children);
        return element;
    }

    function setChildren(target, children) {
        if (!target) {
            return target;
        }
        target.textContent = '';
        appendChild(target, children);
        return target;
    }

    function splitIpString(value) {
        const parts = String(value || '').trim().split('.');
        if (parts.length !== 4) {
            return ['', '', '', ''];
        }
        return parts.map(function(entry) {
            return entry.trim();
        });
    }

    function joinIpParts(parts) {
        if (!Array.isArray(parts)) {
            return '';
        }
        return parts.map(function(entry) {
            return String(entry == null ? '' : entry).trim();
        }).join('.');
    }

    function formatBytes(bytes) {
        const value = Number(bytes || 0);
        const units = ['B', 'kB', 'MB', 'GB'];
        let scaled = value;
        let unitIndex = 0;

        while (scaled >= 1024 && unitIndex < units.length - 1) {
            scaled /= 1024;
            unitIndex++;
        }

        if (unitIndex === 0) {
            return String(Math.round(scaled)) + units[unitIndex];
        }
        return scaled.toFixed(scaled >= 100 ? 0 : 1) + units[unitIndex];
    }

    function formatDuration(seconds) {
        let remaining = Math.max(0, Number(seconds || 0));
        const hours = Math.floor(remaining / 3600);
        remaining -= hours * 3600;
        const minutes = Math.floor(remaining / 60);
        remaining -= minutes * 60;
        return [hours, minutes, Math.floor(remaining)].map(function(entry) {
            return entry < 10 ? '0' + entry : String(entry);
        }).join(':');
    }

    function formatFrequency(hz) {
        const value = Number(hz || 0);
        if (!value) {
            return '-';
        }
        if (value >= 1000000) {
            return (value / 1000000).toFixed(3).replace(/\.?0+$/, '') + ' MHz';
        }
        if (value >= 1000) {
            return (value / 1000).toFixed(1).replace(/\.?0+$/, '') + ' kHz';
        }
        return String(value) + ' Hz';
    }

    function toneClass(tone) {
        if (!tone) {
            return '';
        }
        return ' badge-' + tone;
    }

    async function request(method, path, data, options) {
        if (typeof path !== 'string' || path[0] !== '/') {
            throw new Error('Absolute API path required: ' + String(path));
        }

        const settings = options || {};
        const timeoutMs = settings.timeoutMs || DEFAULT_TIMEOUT_MS;
        const controller = new AbortController();
        const timer = global.setTimeout(function() {
            controller.abort();
        }, timeoutMs);

        const fetchOptions = {
            method: method,
            cache: 'no-store',
            signal: controller.signal,
            headers: {
                'Accept': 'application/json, text/plain;q=0.9, */*;q=0.8'
            }
        };

        if (data !== undefined) {
            fetchOptions.headers['Content-Type'] = 'application/json';
            fetchOptions.body = JSON.stringify(data);
        }

        try {
            const response = await fetch(path, fetchOptions);
            const contentType = response.headers.get('content-type') || '';
            const payload = contentType.indexOf('application/json') >= 0
                ? await response.json()
                : await response.text();

            if (!response.ok) {
                const message = typeof payload === 'string'
                    ? payload
                    : (payload && (payload.error || payload.message || payload.state)) || response.statusText;
                const error = new Error(message || ('HTTP ' + response.status));
                error.status = response.status;
                error.payload = payload;
                throw error;
            }

            return payload;
        } catch (error) {
            if (error.name === 'AbortError') {
                const timeoutError = new Error('Request timeout for ' + path);
                timeoutError.status = 0;
                throw timeoutError;
            }
            throw error;
        } finally {
            global.clearTimeout(timer);
        }
    }

    WebUI.constants = WebUI.constants || {};
    WebUI.constants.SECRET_UNCHANGED = SECRET_UNCHANGED;

    WebUI.util = WebUI.util || {
        q: function(selector, root) { return (root || document).querySelector(selector); },
        qa: function(selector, root) { return Array.from((root || document).querySelectorAll(selector)); },
        make: make,
        setChildren: setChildren,
        appendChild: appendChild,
        show: function(element, visible) {
            if (element) {
                element.classList.toggle('hidden', !visible);
            }
        },
        setText: function(element, text) {
            if (element) {
                element.textContent = text == null ? '' : String(text);
            }
        },
        splitIpString: splitIpString,
        joinIpParts: joinIpParts,
        formatBytes: formatBytes,
        formatDuration: formatDuration,
        formatFrequency: formatFrequency,
        badge: function(text, tone) {
            return make('span', { class: 'badge' + toneClass(tone), text: text });
        },
        pill: function(text, extraClass) {
            return make('span', { class: 'pill' + (extraClass ? ' ' + extraClass : ''), text: text });
        },
        toNumber: function(value, fallback) {
            const parsed = Number(value);
            return Number.isFinite(parsed) ? parsed : fallback;
        },
        toBooleanConfig: function(value) {
            return value ? '1' : '0';
        },
        sameString: function(a, b) {
            return String(a || '').trim() === String(b || '').trim();
        }
    };

    WebUI.api = {
        request: request,
        get: function(path, options) {
            return request('GET', path, undefined, options);
        },
        post: function(path, data, options) {
            return request('POST', path, data, options);
        },
        tryGet: async function(path, fallback, options) {
            try {
                return await request('GET', path, undefined, options);
            } catch (error) {
                return fallback;
            }
        }
    };
})(window);
