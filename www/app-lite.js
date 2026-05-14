(function() {
    function toCamelCase(value) {
        return String(value).replace(/-([a-z])/g, (_, char) => char.toUpperCase());
    }

    function normalizeSelector(selector) {
        if (typeof selector !== 'string') {
            return selector;
        }
        return selector
            .replace(/^:submit(.*)$/g, 'button[type=\"submit\"]$1,input[type=\"submit\"]$1')
            .replace(/^:input(.*)$/g, 'input$1,select$1,textarea$1,button$1')
            .replace(/:selected\b/g, ':checked');
    }

    function toArray(value) {
        if (value == null) {
            return [];
        }
        if (value instanceof MiniQuery) {
            return Array.from(value);
        }
        if (Array.isArray(value)) {
            return value.flatMap(toArray);
        }
        if (value instanceof NodeList || value instanceof HTMLCollection) {
            return Array.from(value);
        }
        return [value];
    }

    function toNode(content) {
        if (content instanceof Node) {
            return content;
        }
        if (content instanceof MiniQuery) {
            return content[0] || null;
        }
        if (typeof content === 'string' || typeof content === 'number') {
            return document.createTextNode(String(content));
        }
        return null;
    }

    function storeData(el) {
        if (!el.__appLiteData) {
            Object.defineProperty(el, '__appLiteData', {
                value: {},
                enumerable: false,
                configurable: false,
                writable: false
            });
        }
        return el.__appLiteData;
    }

    class MiniQuery extends Array {
        constructor(items) {
            super();
            this.push(...items.filter(Boolean));
        }

        each(fn) {
            this.forEach((el, index) => fn.call(el, index, el));
            return this;
        }

        get(index) {
            if (typeof index === 'undefined') {
                return Array.from(this);
            }
            return this[index];
        }

        eq(index) {
            const resolved = index < 0 ? this.length + index : index;
            return new MiniQuery(resolved >= 0 && resolved < this.length ? [this[resolved]] : []);
        }

        find(selector) {
            const normalized = normalizeSelector(selector);
            const found = [];
            this.forEach((el) => {
                found.push(...el.querySelectorAll(normalized));
            });
            return new MiniQuery(found);
        }

        filter(selectorOrFn) {
            if (typeof selectorOrFn === 'function') {
                return new MiniQuery(Array.from(this).filter((el, index) => selectorOrFn.call(el, index, el)));
            }
            const normalized = normalizeSelector(selectorOrFn);
            return new MiniQuery(Array.from(this).filter((el) => el.matches(normalized)));
        }

        children(selector) {
            const children = [];
            this.forEach((el) => {
                children.push(...el.children);
            });
            return selector ? new MiniQuery(children).filter(selector) : new MiniQuery(children);
        }

        parent(selector) {
            const parents = [];
            this.forEach((el) => {
                if (el.parentElement && !parents.includes(el.parentElement)) {
                    parents.push(el.parentElement);
                }
            });
            return selector ? new MiniQuery(parents).filter(selector) : new MiniQuery(parents);
        }

        closest(selector) {
            const matches = [];
            const normalized = normalizeSelector(selector);
            this.forEach((el) => {
                const match = el.closest(normalized);
                if (match && !matches.includes(match)) {
                    matches.push(match);
                }
            });
            return new MiniQuery(matches);
        }

        add(other) {
            return new MiniQuery([...this, ...toArray(other)]);
        }

        index(element) {
            return this.findIndex((candidate) => candidate === element);
        }

        append(content) {
            const items = toArray(content);
            this.forEach((parent, parentIndex) => {
                items.forEach((item) => {
                    const node = toNode(item);
                    if (!node) {
                        return;
                    }
                    parent.appendChild(parentIndex === 0 ? node : node.cloneNode(true));
                });
            });
            return this;
        }

        appendTo(target) {
            $(target).append(this);
            return this;
        }

        empty() {
            this.forEach((el) => {
                el.textContent = '';
            });
            return this;
        }

        text(value) {
            if (typeof value === 'undefined') {
                return this[0] ? this[0].textContent : '';
            }
            this.forEach((el) => {
                el.textContent = value;
            });
            return this;
        }

        html(value) {
            if (typeof value === 'undefined') {
                return this[0] ? this[0].innerHTML : '';
            }
            this.forEach((el) => {
                el.innerHTML = value;
            });
            return this;
        }

        val(value) {
            if (typeof value === 'undefined') {
                return this[0] ? this[0].value : undefined;
            }
            this.forEach((el) => {
                el.value = value;
            });
            return this;
        }

        scrollTop(value) {
            if (typeof value === 'undefined') {
                return this[0] ? this[0].scrollTop : 0;
            }
            this.forEach((el) => {
                el.scrollTop = value;
            });
            return this;
        }

        prop(name, value) {
            if (typeof value === 'undefined') {
                return this[0] ? this[0][name] : undefined;
            }
            this.forEach((el) => {
                el[name] = value;
            });
            return this;
        }

        attr(name, value) {
            if (typeof value === 'undefined') {
                return this[0] ? this[0].getAttribute(name) : undefined;
            }
            this.forEach((el) => {
                if (value === false || value == null) {
                    el.removeAttribute(name);
                } else {
                    el.setAttribute(name, value === true ? '' : String(value));
                }
            });
            return this;
        }

        data(name, value) {
            if (!this[0]) {
                return typeof value === 'undefined' ? undefined : this;
            }
            const key = String(name);
            const datasetKey = toCamelCase(key);
            if (typeof value === 'undefined') {
                const local = storeData(this[0]);
                if (Object.prototype.hasOwnProperty.call(local, key)) {
                    return local[key];
                }
                return this[0].dataset ? this[0].dataset[datasetKey] : undefined;
            }
            this.forEach((el) => {
                storeData(el)[key] = value;
            });
            return this;
        }

        addClass(names) {
            const list = String(names || '').split(/\s+/).filter(Boolean);
            this.forEach((el) => el.classList.add(...list));
            return this;
        }

        removeClass(names) {
            const list = String(names || '').split(/\s+/).filter(Boolean);
            this.forEach((el) => el.classList.remove(...list));
            return this;
        }

        toggleClass(name, force) {
            this.forEach((el) => el.classList.toggle(name, force));
            return this;
        }

        hasClass(name) {
            return !!this[0] && this[0].classList.contains(name);
        }

        show() {
            this.forEach((el) => {
                el.style.display = '';
            });
            return this;
        }

        hide() {
            this.forEach((el) => {
                el.style.display = 'none';
            });
            return this;
        }

        is(selector) {
            if (!this[0]) {
                return false;
            }
            return this[0].matches(normalizeSelector(selector));
        }

        on(eventName, selectorOrHandler, maybeHandler) {
            const delegated = typeof selectorOrHandler === 'string';
            const selector = delegated ? normalizeSelector(selectorOrHandler) : null;
            const handler = delegated ? maybeHandler : selectorOrHandler;
            const events = String(eventName || '').split(/\s+/).filter(Boolean);

            this.forEach((el) => {
                events.forEach((singleEvent) => {
                    el.addEventListener(singleEvent, (event) => {
                        if (!delegated) {
                            handler.call(el, event);
                            return;
                        }
                        const match = event.target.closest(selector);
                        if (match && el.contains(match)) {
                            handler.call(match, event);
                        }
                    });
                });
            });
            return this;
        }

        trigger(eventName) {
            this.forEach((el) => {
                el.dispatchEvent(new Event(eventName, { bubbles: true }));
            });
            return this;
        }

        insertAfter(target) {
            const targetElement = $(target)[0];
            if (!targetElement || !targetElement.parentNode) {
                return this;
            }
            const parent = targetElement.parentNode;
            let cursor = targetElement.nextSibling;
            this.forEach((el) => {
                parent.insertBefore(el, cursor);
                cursor = el.nextSibling;
            });
            return this;
        }

        map(callback) {
            const values = Array.from(this).map((el, index) => callback.call(el, index, el));
            return {
                get() {
                    return values;
                }
            };
        }

        serializeArray() {
            const form = this[0];
            if (!(form instanceof HTMLFormElement)) {
                return [];
            }
            const result = [];
            Array.from(form.elements).forEach((field) => {
                if (!field.name || field.disabled) {
                    return;
                }
                if ((field.type === 'checkbox' || field.type === 'radio') && !field.checked) {
                    return;
                }
                if (field.tagName === 'SELECT' && field.multiple) {
                    Array.from(field.selectedOptions).forEach((option) => {
                        result.push({ name: field.name, value: option.value || '' });
                    });
                    return;
                }
                result.push({ name: field.name, value: field.value || '' });
            });
            return result;
        }

        serializeObject() {
            const object = {};
            this.serializeArray().forEach((entry) => {
                if (Object.prototype.hasOwnProperty.call(object, entry.name)) {
                    if (!Array.isArray(object[entry.name])) {
                        object[entry.name] = [object[entry.name]];
                    }
                    object[entry.name].push(entry.value);
                } else {
                    object[entry.name] = entry.value;
                }
            });
            return object;
        }

        tooltip() {
            return this;
        }

        modal(action) {
            this.forEach((el) => {
                if (action === 'hide') {
                    el.classList.remove('show');
                    el.style.display = 'none';
                } else {
                    el.classList.add('show');
                    el.style.display = 'flex';
                }
            });
            return this;
        }
    }

    function createElementFromHtml(html, attrs) {
        const template = document.createElement('template');
        template.innerHTML = html.trim();
        const node = template.content.firstElementChild || template.content.firstChild;
        if (!node) {
            return new MiniQuery([]);
        }
        if (attrs && node instanceof HTMLElement) {
            Object.keys(attrs).forEach((key) => {
                const value = attrs[key];
                if (key === 'class') {
                    node.className = value;
                } else if (key === 'text') {
                    node.textContent = value;
                } else if (key === 'html') {
                    node.innerHTML = value;
                } else if (key === 'checked' || key === 'selected' || key === 'disabled') {
                    node[key] = !!value;
                } else {
                    node.setAttribute(key, value);
                }
            });
        }
        return new MiniQuery([node]);
    }

    function $(input, attrs) {
        if (typeof input === 'function') {
            if (document.readyState === 'loading') {
                document.addEventListener('DOMContentLoaded', input, { once: true });
            } else {
                input();
            }
            return new MiniQuery([]);
        }

        if (typeof input === 'string' && input.trim().startsWith('<')) {
            return createElementFromHtml(input, attrs);
        }

        if (typeof input === 'string') {
            return new MiniQuery(Array.from(document.querySelectorAll(normalizeSelector(input))));
        }

        return new MiniQuery(toArray(input));
    }

    $.fn = MiniQuery.prototype;

    $.each = function(collection, callback) {
        if (Array.isArray(collection)) {
            collection.forEach((value, index) => callback.call(value, index, value));
            return;
        }
        Object.keys(collection || {}).forEach((key) => callback.call(collection[key], key, collection[key]));
    };

    function wrapPromise(promise) {
        const api = {
            done(fn) {
                promise = promise.then((payload) => {
                    fn(payload.data, payload.response);
                    return payload;
                });
                return api;
            },
            fail(fn) {
                promise = promise.catch((error) => {
                    fn(error);
                    throw error;
                });
                return api;
            },
            always(fn) {
                promise = promise.finally(fn);
                return api;
            },
            then(fn) {
                return promise.then(fn);
            },
            catch(fn) {
                return promise.catch(fn);
            }
        };
        return api;
    }

    $.ajax = function(options) {
        const config = typeof options === 'string' ? { url: options } : Object.assign({ method: 'GET' }, options || {});
        const headers = new Headers(config.headers || {});
        if (config.contentType) {
            headers.set('Content-Type', config.contentType);
        }
        const controller = config.timeout ? new AbortController() : null;
        if (controller) {
            setTimeout(() => controller.abort(), config.timeout);
        }
        const request = fetch(config.url, {
            method: config.method || 'GET',
            headers,
            body: config.data,
            signal: controller ? controller.signal : undefined
        }).then(async (response) => {
            const wantsJson = config.dataType === 'json' || (response.headers.get('content-type') || '').includes('application/json');
            const data = wantsJson ? await response.json() : await response.text();
            if (!response.ok) {
                const error = new Error(response.statusText || 'Request failed');
                error.response = response;
                error.data = data;
                throw error;
            }
            return { data, response };
        });
        return wrapPromise(request);
    };

    $.getJSON = function(url, success) {
        const request = $.ajax({ url, dataType: 'json' });
        if (typeof success === 'function') {
            request.done(success);
        }
        return request;
    };

    $.post = function(url, data) {
        const options = { url, method: 'POST' };
        if (typeof data !== 'undefined') {
            options.data = data;
        }
        return $.ajax(options);
    };

    document.addEventListener('click', (event) => {
        const toggle = event.target.closest('.dropdown-toggle');
        if (toggle) {
            const menu = toggle.parentElement ? toggle.parentElement.querySelector('.dropdown-menu') : null;
            if (menu) {
                event.preventDefault();
                menu.classList.toggle('show');
            }
            return;
        }

        document.querySelectorAll('.dropdown-menu.show').forEach((menu) => {
            if (!menu.contains(event.target)) {
                menu.classList.remove('show');
            }
        });
    });

    document.addEventListener('change', (event) => {
        const input = event.target;
        if (!(input instanceof HTMLInputElement)) {
            return;
        }
        const label = input.closest('.btn');
        if (!label) {
            return;
        }
        if (input.type === 'radio' && input.name) {
            document.querySelectorAll('input[name="' + CSS.escape(input.name) + '"]').forEach((radio) => {
                const radioLabel = radio.closest('.btn');
                if (radioLabel) {
                    radioLabel.classList.toggle('active', radio.checked);
                }
            });
            return;
        }
        if (input.type === 'checkbox') {
            label.classList.toggle('active', input.checked);
        }
    });

    window.$ = $;
})();
