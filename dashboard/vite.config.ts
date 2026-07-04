import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// Relative base so the built bundle works when served from any subpath
// (e.g. a static file host on the demo laptop). `global: globalThis` keeps the
// browser build of mqtt.js happy under Vite.
export default defineConfig({
  base: './',
  plugins: [react()],
  define: { global: 'globalThis' },
  server: { port: 5173 },
});
