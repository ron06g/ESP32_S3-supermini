// ===========================================================================
//  S3-KBD — Amorçage PWA : enregistrement du Service Worker + bouton
//  « Installer ». Tout en CHEMINS RELATIFS (aucune URL absolue).
// ===========================================================================
(function () {
  'use strict';

  const log = (msg) => {
    if (typeof window.logLine === 'function') window.logLine('in', 'PWA: ' + msg);
    else console.log('[PWA]', msg);
  };

  // --- 1) Service Worker (chemin relatif → portée = dossier courant) ---
  if ('serviceWorker' in navigator) {
    window.addEventListener('load', () => {
      navigator.serviceWorker.register('./sw.js', { scope: './' })
        .then((reg) => log('service worker enregistré (scope ' + reg.scope + ')'))
        .catch((err) => log('échec service worker : ' + err.message));
    });
  } else {
    log('service worker non supporté par ce navigateur');
  }

  // --- 2) Bouton « Installer » (invite native quand disponible) ---
  let deferred = null;
  const btn = () => document.getElementById('btnInstall');

  const standalone =
    window.matchMedia('(display-mode: standalone)').matches ||
    window.navigator.standalone === true;

  window.addEventListener('beforeinstallprompt', (e) => {
    e.preventDefault();          // on garde la main sur le moment de l'invite
    deferred = e;
    const b = btn();
    if (b && !standalone) { b.classList.remove('hidden'); log('installable'); }
  });

  document.addEventListener('click', async (e) => {
    const b = btn();
    if (!b || e.target !== b) return;
    if (!deferred) return;
    deferred.prompt();
    const { outcome } = await deferred.userChoice;
    log('invite installation : ' + outcome);
    deferred = null;
    b.classList.add('hidden');
  });

  window.addEventListener('appinstalled', () => {
    log('application installée');
    const b = btn();
    if (b) b.classList.add('hidden');
  });
})();
