(function(global) {
    const WebUI = global.WebUI = global.WebUI || {};
    const util = WebUI.util;

    const links = [
        { id: 'dashboard', label: 'Dashboard', href: '/dashboard.html' },
        { id: 'config', label: 'Config', href: '/config.html' },
        { id: 'advanced', label: 'Advanced', href: '/advanced.html' },
        { id: 'logs', label: 'Logs', href: '/log.html' }
    ];

    WebUI.nav = {
        render: function(activePage) {
            const target = util.q('#top-nav');
            if (!target) {
                return;
            }

            target.className = 'page-nav';
            util.setChildren(target, links.map(function(link) {
                return util.make('a', {
                    class: 'btn ' + (link.id === activePage ? 'btn-primary' : 'btn-outline-secondary'),
                    text: link.label,
                    attrs: {
                        href: link.href,
                        'aria-current': link.id === activePage ? 'page' : null
                    }
                });
            }));
        }
    };
})(window);
