import {resolve, sep} from 'node:path';
import {readFile, stat} from 'node:fs/promises';
import {defineConfig} from 'vite';

// CMake produces the complete Pages artifact, including Emscripten and
// benchmark outputs. Vite is intentionally used as the local preview server;
// it must not rebuild or rewrite those provenance-bound files.
const repositoryRoot = resolve(import.meta.dirname);
const pagesDist = resolve(
  repositoryRoot,
  process.env.DYNARMIC_JS_PAGES_DIST ?? 'build-em/examples/browser/dist',
);

export default defineConfig(({isPreview}) => {
  if (!isPreview) throw new Error('Build engines and source together with npm run build; use npm run pages:preview to serve them.');
  return {
    root: repositoryRoot,
    // Exercise the real project path, including absolute 404 recovery links.
    base: '/dynarmic.js/',
    appType: 'mpa',
    publicDir: false,
    plugins: [{
      name: 'pages-not-found',
      async configurePreviewServer(server) {
        for (const name of ['index.html', '404.html']) {
          if (!(await stat(resolve(pagesDist, name)).catch(() => null))?.isFile()) {
            throw new Error(`Missing ${name} in ${pagesDist}. Run npm run build after configuring CMake, or set DYNARMIC_JS_PAGES_DIST to a complete site.`);
          }
        }
        return () => server.middlewares.use(async (request, response, next) => {
          try {
            // Vite's HTML fallback runs before this hook and its HTML file
            // handler runs after it. Let existing HTML (including /index.html)
            // reach that handler; only missing requests use the custom 404.
            const pathname = decodeURIComponent(new URL(request.url, 'http://localhost').pathname);
            const file = resolve(pagesDist, `.${pathname}`);
            if (file.startsWith(pagesDist + sep) && file.endsWith('.html') &&
                (await stat(file).catch(() => null))?.isFile()) return next();
            const body = await readFile(resolve(pagesDist, '404.html'));
            response.writeHead(404, {'Content-Type': 'text/html; charset=utf-8'});
            response.end(request.method === 'HEAD' ? undefined : body);
          } catch (error) { next(error); }
        });
      },
    }],
    build: {
      outDir: pagesDist,
      emptyOutDir: false,
    },
    preview: {
      host: '127.0.0.1',
      port: 4173,
      strictPort: true,
    },
  };
});
