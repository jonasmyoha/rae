// Phase 1 HTTP reference (benchmarks/servers/spec/Http.md), tuned: Bun.serve
// with its native route table, no dependencies. PORT (default 8080); WORKERS
// (default 1: one process; more: that many processes on the same port with
// reusePort).
const port = Number(process.env.PORT || 8080);
const workers = Number(process.env.WORKERS || 1);

if (workers > 1 && !process.env.BUN_SERVER_CHILD) {
  const children = [];
  for (let i = 0; i < workers; i++) {
    children.push(
      Bun.spawn([process.execPath, import.meta.path], {
        env: { ...process.env, BUN_SERVER_CHILD: "1" },
        stdout: "inherit",
        stderr: "inherit",
      }),
    );
  }
  const stop = () => {
    for (const child of children) child.kill();
    process.exit(0);
  };
  process.on("SIGTERM", stop);
  process.on("SIGINT", stop);
  console.log(`listening on ${port} with ${workers} workers`);
} else {
  const plaintextHeaders = { Server: "Bun", "Content-Type": "text/plain" };
  const jsonHeaders = { Server: "Bun", "Content-Type": "application/json" };
  Bun.serve({
    port,
    hostname: "127.0.0.1",
    reusePort: workers > 1,
    // Bun's own router matches these in native code, before any JavaScript runs
    routes: {
      "/plaintext": () => new Response("Hello, World!", { headers: plaintextHeaders }),
      "/json": () => new Response(JSON.stringify({ message: "Hello, World!" }), { headers: jsonHeaders }),
    },
    fetch() {
      return new Response("Not Found", { status: 404, headers: plaintextHeaders });
    },
  });
  if (workers <= 1) console.log(`listening on ${port}`);
}
