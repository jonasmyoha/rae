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

export class StatsStore {
  private metricsPath: string;

  constructor(metricsPath = path.resolve(process.cwd(), "data", "runtime_metrics.jsonl")) {
    this.metricsPath = metricsPath;
    mkdirSync(path.dirname(this.metricsPath), { recursive: true });
    if (!existsSync(this.metricsPath)) {
      writeFileSync(this.metricsPath, "");
    }
  }

  record(metricName: string, metricValue: number, metadata: MetricMetadata = {}) {
    const now = new Date();
    const todayStr = now.toISOString().split("T")[0]; // YYYY-MM-DD

    const entries = this.readAll();
    
    // Find if we already have an entry for this metric today
    // We look for the last one that matches the name and date
    let index = -1;
    for (let i = entries.length - 1; i >= 0; i--) {
      const entry = entries[i];
      if (entry.metric_name === metricName && entry.timestamp.startsWith(todayStr)) {
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
