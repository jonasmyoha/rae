// Concurrency test for ExampleRunner. Uses `sleep` as the stand-in example
// command so it exercises the real spawn / roster / kill paths without needing
// a compiler or a display.
//
//   bun run scripts/testExampleRunner.ts
import { ExampleRunner } from "../src/server/exampleRunner";
import type { RaeDevtoolsConfig } from "../src/server/config";
import type { ServerEvent } from "../src/shared/types";

const config: RaeDevtoolsConfig = {
  compilerPath: process.cwd(),
  port: 0,
  examplesPath: "examples",
  defaultTarget: "fake",
  targets: [
    {
      id: "fake",
      label: "Fake",
      exampleRunCommand: "sleep 30",
      exampleWatchCommand: "sleep 30"
    }
  ]
};

const events: ServerEvent[] = [];
const runner = new ExampleRunner(config, (event) => events.push(event));

let failures = 0;
function check(label: string, condition: boolean) {
  if (condition) {
    console.log(`  ok   ${label}`);
  } else {
    failures += 1;
    console.log(`  FAIL ${label}`);
  }
}

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

console.log("two apps run concurrently");
await runner.run("a/main.rae", { mode: "watch", exampleId: "a" });
await runner.run("b/main.rae", { mode: "watch", exampleId: "b" });
await sleep(150);
let runs = runner.list();
check("both runs are live", runs.length === 2);
check("both are watch sessions", runs.every((run) => run.mode === "watch"));
check("starting b did not complete a", !events.some((e) => e.type === "example-run-completed"));

console.log("restarting one app replaces only its own run");
const idA = runs.find((run) => run.exampleId === "a")!.runId;
const idB = runs.find((run) => run.exampleId === "b")!.runId;
await runner.run("a/main.rae", { mode: "watch", exampleId: "a" });
await sleep(150);
runs = runner.list();
check("still exactly two runs", runs.length === 2);
check("a got a fresh run id", runs.find((run) => run.exampleId === "a")!.runId !== idA);
check("b kept its run id", runs.find((run) => run.exampleId === "b")!.runId === idB);

console.log("stop is per run");
await runner.stop(idB);
await sleep(150);
runs = runner.list();
check("only a survives", runs.length === 1 && runs[0]!.exampleId === "a");

console.log("stop with no id stops everything");
await runner.run("c/main.rae", { mode: "run", exampleId: "c" });
await sleep(150);
check("two runs before stop-all", runner.list().length === 2);
await runner.stop();
await sleep(150);
check("roster is empty", runner.list().length === 0);

console.log("roster is broadcast");
const rosters = events.filter((e) => e.type === "example-runs");
check("roster events were emitted", rosters.length > 0);
check(
  "final roster event is empty",
  (rosters[rosters.length - 1] as { runs: unknown[] }).runs.length === 0
);

console.log(failures === 0 ? "\nAll checks passed." : `\n${failures} check(s) failed.`);
process.exit(failures === 0 ? 0 : 1);
