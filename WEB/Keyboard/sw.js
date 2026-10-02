// ===========================================================================
//  S3-KBD — Service Worker (PWA).
//  CHEMINS RELATIFS uniquement : la portée (scope) du SW est déduite de son
//  emplacement, donc l'appli fonctionne quel que soit le sous-dossier
//  d'hébergement (racine du domaine OU /un/sous/dossier/). Aucune URL absolue
//  n'est nécessaire, seul le HTTPS est requis (déjà en place).
//
//  Stratégie : RÉSEAU D'ABORD, cache en repli.
//   - en ligne  : on sert toujours la dernière version (pas de fichier figé),
//                 et on rafraîchit la copie en cache au passage ;
//   - hors ligne: on sert la copie mise en cache (installation utilisable).
//  Cohérent avec un outil de pilotage qui doit rester synchro du contrat GATT.
// ===========================================================================
const CACHE = 's3kbd-v24';           // ← incrémenter pour forcer un rafraîchissement
const SHELL = [
  './',
  './index.html',
  './app.js',
  './pwa.js',
  './style.css',
  './manifest.webmanifest',
  './icons/icon-192.png',
  './icons/icon-512.png',
  './icons/icon-maskable-512.png',
];

// --- Installation : pré-cache de la coquille applicative ---
self.addEventListener('install', (e) => {
  self.skipWaiting();
  e.waitUntil(
    caches.open(CACHE).then((c) => c.addAll(SHELL)).catch(() => {})
  );
});

// --- Activation : purge des anciens caches + prise de contrôle immédiate ---
self.addEventListener('activate', (e) => {
  e.waitUntil((async () => {
    const keys = await caches.keys();
    await Promise.all(keys.filter((k) => k !== CACHE).map((k) => caches.delete(k)));
    await self.clients.claim();
  })());
});

// --- Requêtes : réseau d'abord (même origine, GET), cache en repli ---
self.addEventListener('fetch', (e) => {
  const req = e.request;
  if (req.method !== 'GET') return;                       // laisse passer POST, etc.
  const url = new URL(req.url);
  if (url.origin !== self.location.origin) return;        // pas de cache cross-origin

  e.respondWith((async () => {
    try {
      // cache:'no-cache' = revalide aupres du serveur (sinon le fetch du SW peut
      // renvoyer une copie du cache HTTP et figer app.js sur un site sans en-tetes).
      const net = await fetch(req, { cache: 'no-cache' });
      if (net && net.ok && net.type === 'basic') {
        const copy = net.clone();
        caches.open(CACHE).then((c) => c.put(req, copy)).catch(() => {});
      }
      return net;
    } catch (err) {
      const cached = await caches.match(req);
      if (cached) return cached;
      if (req.mode === 'navigate') {
        const home = (await caches.match('./index.html')) || (await caches.match('./'));
        if (home) return home;
      }
      throw err;
    }
  })());
});
