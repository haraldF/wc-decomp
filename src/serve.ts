// Serves the web build (out/web) on http://localhost with the headers that make the page
// cross-origin isolated, which the emulation's thread (SharedArrayBuffer) needs.
// Usage: node src/serve.ts [dir] [--port n]

import { createReadStream, existsSync, statSync } from "node:fs";
import { createServer } from "node:http";
import path from "node:path";
import { parseArgs } from "node:util";

const root = path.resolve(import.meta.dirname, "..");
const { positionals, values } = parseArgs({
  allowPositionals: true,
  options: { port: { type: "string", default: "8080" } },
});
const dir = path.resolve(positionals[0] ?? path.join(root, "out", "web"));
if (!existsSync(path.join(dir, "index.html"))) {
  console.error(`serve: no index.html in ${dir} (npm run web builds it)`);
  process.exit(1);
}

const types: Record<string, string> = {
  ".html": "text/html; charset=utf-8", ".js": "text/javascript", ".wasm": "application/wasm",
  ".json": "application/json", ".png": "image/png", ".ico": "image/x-icon",
};

createServer((req, res) => {
  const url = new URL(req.url ?? "/", "http://localhost");
  const file = path.join(dir, path.normalize(decodeURIComponent(url.pathname)).replace(/^([/\\])+/, ""));
  const target = file.endsWith(path.sep) || (existsSync(file) && statSync(file).isDirectory()) ? path.join(file, "index.html") : file;
  if (!target.startsWith(dir + path.sep) || !existsSync(target)) {
    res.writeHead(404).end("not found");
    return;
  }
  res.writeHead(200, {
    "Content-Type": types[path.extname(target)] ?? "application/octet-stream",
    "Cross-Origin-Opener-Policy": "same-origin",
    "Cross-Origin-Embedder-Policy": "require-corp",
    "Cache-Control": "no-cache",
  });
  createReadStream(target).pipe(res);
}).listen(Number(values.port), () => console.log(`serving ${dir} at http://localhost:${values.port}/`));
