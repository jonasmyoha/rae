// Phase 1 HTTP reference (benchmarks/servers/spec/Http.md): Bun.serve, no
// dependencies. PORT (default 8080); WORKERS (default 1: one process; more:
// that many processes on the same port with reusePort).
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
  Bun.serve({
    port,
    hostname: "127.0.0.1",
    reusePort: workers > 1,
    fetch(request) {
      const path = new URL(request.url).pathname;
      if (path === "/plaintext") {
        return new Response("Hello, World!", { headers: { Server: "Bun", "Content-Type": "text/plain" } });
      }
      if (path === "/json") {
        return new Response(JSON.stringify({ message: "Hello, World!" }), {
          headers: { Server: "Bun", "Content-Type": "application/json" },
        });
      }
      return new Response("Not Found", { status: 404, headers: { Server: "Bun", "Content-Type": "text/plain" } });
    },
  });
  if (workers <= 1) console.log(`listening on ${port}`);
}
