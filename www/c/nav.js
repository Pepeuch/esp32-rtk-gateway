(function(global) {
    const app = global.WebUI || global.ConfigPage || {};
    global.WebUI = app;
    global.ConfigPage = app;

    const links = [
        { href: '/dashboard.html', label: 'Dashboard', id: 'dashboard' },
        { href: '/config.html', label: 'Config', id: 'config' },
        { href: '/advanced.html', label: 'Advanced', id: 'advanced' },
        { href: '/log.html', label: 'Logs', id: 'log' }
    ];

    app.initNav = function(options) {
        const page = (options && options.page) || $('body').data('page') || '';
        const roots = $('.shared-nav');

        roots.each(function() {
            const root = $(this);
            root.empty();

            links.forEach(function(link) {
                const active = link.id === page;
                root.append($('<a>', {
                    href: link.href,
                    class: 'btn btn-sm ' + (active ? 'btn-primary' : 'btn-outline-secondary') + ' mr-2 mb-2',
                    text: link.label,
                    'aria-current': active ? 'page' : null
                }));
            });
        });
    };
})(window);
