import { mkdirSync, readFileSync, writeFileSync, appendFileSync, existsSync } from "node:fs";
import path from "node:path";

type MetricMetadata = Record<string, unknown>;

export type RuntimeMetricEntry = {
  timestamp: string;
  metric_name: string;
  metric_value: number;
  metadata: MetricMetadata;
};

export type TestRunStats = {
  runId: string;
  durationMs: number;
  success: boolean;
  passed: number;
  failed: number;
  targetId: string;
  targetLabel: string;
};

export type BuildRunStats = {
  runId: string;
  durationMs: number;
  success: boolean;
  command: string;
  targetId: string;
  targetLabel: string;
};

export type ExampleBuildStats = {
  runId: string;
  exampleId: string;
  entry: string;
  targetId: string;
  profile?: string;
  totalMs: number;
  emitMs: number;
  ccMs: number;
  lines: number;
  projectLines: number;
  modules: number;
  msPerKloc: number;
};

export class StatsStore {
  private metricsPath: string;

  constructor(metricsPath = path.resolve(process.cwd(), "data", "runtime_metrics.jsonl")) {
    this.metricsPath = metricsPath;
    mkdirSync(path.dirname(this.metricsPath), { recursive: true });
    if (!existsSync(this.metricsPath)) {
      writeFileSync(this.metricsPath, "");
    }
  }

  /** One entry per metric per day; with `keyExampleId`, per metric per day
   * PER EXAMPLE (metadata.exampleId), so per-app metrics don't overwrite each
   * other. */
  record(metricName: string, metricValue: number, metadata: MetricMetadata = {}, keyExampleId?: string) {
    const now = new Date();
    const todayStr = now.toISOString().split("T")[0]; // YYYY-MM-DD

    const entries = this.readAll();
    
    // Find if we already have an entry for this metric today
    // We look for the last one that matches the name and date
    let index = -1;
    for (let i = entries.length - 1; i >= 0; i--) {
      const entry = entries[i];
      if (entry.metric_name === metricName && entry.timestamp.startsWith(todayStr)) {
        if (keyExampleId !== undefined && entry.metadata?.exampleId !== keyExampleId) continue;
        index = i;
        break;
      }
    }

    const newEntry: RuntimeMetricEntry = {
      timestamp: now.toISOString(),
      metric_name: metricName,
      metric_value: metricValue,
      metadata
    };

    if (index !== -1) {
      // Update existing entry for today
      entries[index] = newEntry;
      this.writeAll(entries);
    } else {
      // Append new entry
      appendFileSync(this.metricsPath, JSON.stringify(newEntry) + "\n");
    }
  }

  recordTestRun(data: TestRunStats) {
    const metadata = {
      runId: data.runId,
      success: data.success,
      passed: data.passed,
      failed: data.failed,
      targetId: data.targetId,
      targetLabel: data.targetLabel
    };
    this.record("tests.duration_ms", data.durationMs, metadata);
    this.record("tests.failed", data.failed, metadata);
    this.record("tests.passed", data.passed, metadata);
  }

  recordBuildRun(data: BuildRunStats) {
    const metadata = {
      runId: data.runId,
      success: data.success,
      command: data.command,
      targetId: data.targetId,
      targetLabel: data.targetLabel
    };
    this.record("builds.duration_ms", data.durationMs, metadata);
    this.record("builds.success", data.success ? 1 : 0, metadata);
  }

  /** Per-example build timing from the compiler's @@RAE_BUILD_TIME@@ line: the
   * full build (emit + cc) and the size-normalised ms per 1,000 processed
   * lines, one entry per example per day (the daily dedupe is keyed on the
   * example, not just the metric name, so a Run-all batch keeps every app). */
  recordExampleBuild(data: ExampleBuildStats) {
    const metadata = {
      runId: data.runId,
      exampleId: data.exampleId,
      entry: data.entry,
      targetId: data.targetId,
      profile: data.profile,
      emitMs: data.emitMs,
      ccMs: data.ccMs,
      lines: data.lines,
      projectLines: data.projectLines,
      modules: data.modules
    };
    this.record("examples.build_ms", data.totalMs, metadata, data.exampleId);
    this.record("examples.build_ms_per_kloc", data.msPerKloc, metadata, data.exampleId);
  }

  /** The latest recorded value of `metricName` for every example, keyed by
   * exampleId — what the Featured list shows next to each app. */
  latestPerExample(metricName: string) {
    const out: Record<string, { timestamp: string; value: number; metadata: MetricMetadata }> = {};
    for (const e of this.readAll()) {
      if (e.metric_name !== metricName) continue;
      const id = e.metadata?.exampleId;
      if (typeof id !== "string") continue;
      out[id] = { timestamp: e.timestamp, value: e.metric_value, metadata: e.metadata };
    }
    return out;
  }

  listRecentMetrics(metricName: string, limit = 20) {
    const all = this.readAll();
    return all
      .filter(e => e.metric_name === metricName)
      .reverse()
      .slice(0, limit)
      .map(e => ({
        timestamp: e.timestamp,
        value: e.metric_value,
        metadata: e.metadata
      }));
  }

  private readAll(): RuntimeMetricEntry[] {
    try {
      const content = readFileSync(this.metricsPath, "utf-8");
      return content
        .split("\n")
        .filter(line => line.trim().length > 0)
        .map(line => JSON.parse(line));
    } catch (e) {
      return [];
    }
  }

  private writeAll(entries: RuntimeMetricEntry[]) {
    const content = entries.map(e => JSON.stringify(e)).join("\n") + "\n";
    writeFileSync(this.metricsPath, content);
  }
}
