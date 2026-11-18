(() => {
    'use strict';
    const button = document.getElementById('style-btn');
    const drawer = document.getElementById('style-drawer');
    if (!button || !drawer) return;
    function setOpen(open) {
        drawer.classList.toggle('is-open', open);
        button.classList.toggle('is-active', open);
        button.setAttribute('aria-expanded', String(open));
        if (open) {
            const selectedTab = drawer.querySelector('.tab-btn.active');
            if (selectedTab?.dataset.target === 'tab-effects' && typeof renderAllCardThumbnails === 'function') {
                renderAllCardThumbnails();
            }
        }
        setTimeout(() => {
            if (typeof refreshViewport === 'function') refreshViewport();
        }, 280);
    }
    button.addEventListener('click', () => {
        const willOpen = !drawer.classList.contains('is-open');
        if (willOpen) {
            for (const [drawerId, buttonId] of [
                ['ai-lyrics-drawer', 'ai-lyrics-btn'],
                ['export-drawer', 'export-btn'],
                ['media-drawer', 'media-btn']
            ]) {
                document.getElementById(drawerId)?.classList.remove('is-open');
                document.getElementById(buttonId)?.classList.remove('is-active');
            }
        }
        setOpen(willOpen);
    });
    window.StyleDrawer = {
        close() { setOpen(false); },
        isOpen() { return drawer.classList.contains('is-open'); }
    };
})();
