(function(global) {
    const app = global.ConfigPage || (global.ConfigPage = {});

    app.initNav = function() {
        $('.config-page-nav a').each(function() {
            const href = $(this).attr('href');
            const active = href && window.location.pathname.endsWith(href.replace(/^\//, ''));
            $(this).toggleClass('btn-primary', !!active)
                .toggleClass('btn-outline-secondary', !active && $(this).hasClass('btn-outline-secondary'))
                .attr('aria-current', active ? 'page' : null);
        });
    };
})(window);
