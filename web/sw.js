// Makes the page cross-origin isolated where the server can't send the headers itself (static
// hosting): the emulation's thread needs SharedArrayBuffer, which browsers only allow then.
// index.html registers it and reloads once; `npm run serve` sends the headers without it.
self.addEventListener("install", () => self.skipWaiting());
self.addEventListener("activate", (e) => e.waitUntil(self.clients.claim()));
self.addEventListener("fetch", (e) => {
  const req = e.request;
  if (req.cache === "only-if-cached" && req.mode !== "same-origin") return;
  e.respondWith(fetch(req).then((res) => {
    if (res.status === 0) return res; // opaque: nothing to add
    const headers = new Headers(res.headers);
    headers.set("Cross-Origin-Opener-Policy", "same-origin");
    headers.set("Cross-Origin-Embedder-Policy", "require-corp");
    headers.set("Cross-Origin-Resource-Policy", "cross-origin");
    return new Response(res.body, { status: res.status, statusText: res.statusText, headers });
  }));
});
