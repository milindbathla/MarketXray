import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173,
    proxy: {
      // Browser clients can reach a remote Vite host while this proxy still
      // connects to the local analytics gateway on that host.
      "/marketxray-ws": {
        target: "ws://127.0.0.1:9001",
        ws: true,
      },
    },
  },
});
