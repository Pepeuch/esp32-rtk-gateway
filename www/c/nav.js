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

    function renderNav(page, targetSelector) {
        const target = $(targetSelector || '#top-nav');
        if (!target.length) return;

        target.empty();

        links.forEach(function(link) {
            const active = link.id === page;
            target.append($('<a>', {
                href: link.href,
                class: 'btn btn-sm ' + (active ? 'btn-primary' : 'btn-outline-secondary') + ' mr-2 mb-2',
                text: link.label,
                'aria-current': active ? 'page' : null
            }));
        });
    }

    app.renderNav = renderNav;
    global.renderNav = renderNav;
})(window);
