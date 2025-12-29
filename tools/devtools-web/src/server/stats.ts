import { mkdirSync } from "node:fs";
import path from "node:path";
import { Database } from "bun:sqlite";

type MetricMetadata = Record<string, unknown>;

export type TestRunStats = {
  runId: string;
  durationMs: number;
  success: boolean;
  passed: number;
  failed: number;
};

export type BuildRunStats = {
  runId: string;
  durationMs: number;
  success: boolean;
  command: string;
};

export class StatsStore {
  private db: Database;

  constructor(dbPath = path.resolve(process.cwd(), "data", "devtools.db")) {
    mkdirSync(path.dirname(dbPath), { recursive: true });
    this.db = new Database(dbPath, { create: true });
    this.bootstrap();
  }

  record(metricName: string, metricValue: number, metadata: MetricMetadata = {}) {
    this.db
      .query(
        `INSERT INTO stats (timestamp, metric_name, metric_value, metadata)
         VALUES ($timestamp, $metric_name, $metric_value, $metadata)`
      )
      .run({
        $timestamp: new Date().toISOString(),
        $metric_name: metricName,
        $metric_value: metricValue,
        $metadata: JSON.stringify(metadata)
      });
  }

  recordTestRun(data: TestRunStats) {
    const metadata = {
      runId: data.runId,
      success: data.success,
      passed: data.passed,
      failed: data.failed
    };
    this.record("tests.duration_ms", data.durationMs, metadata);
    this.record("tests.failed", data.failed, metadata);
    this.record("tests.passed", data.passed, metadata);
  }

  recordBuildRun(data: BuildRunStats) {
    const metadata = {
      runId: data.runId,
      success: data.success,
      command: data.command
    };
    this.record("builds.duration_ms", data.durationMs, metadata);
    this.record("builds.success", data.success ? 1 : 0, metadata);
  }

  listRecentMetrics(metricName: string, limit = 20) {
    const rows = this.db
      .query(
        `SELECT timestamp, metric_value AS value, metadata
         FROM stats
         WHERE metric_name = $metric
         ORDER BY timestamp DESC
         LIMIT $limit`
      )
      .all({
        $metric: metricName,
        $limit: limit
      }) as Array<{ timestamp: string; value: number; metadata: string | null }>;

    return rows.map((row) => ({
      timestamp: row.timestamp,
      value: row.value,
      metadata: row.metadata ? JSON.parse(row.metadata) : null
    }));
  }

  private bootstrap() {
    this.db
      .query(
        `CREATE TABLE IF NOT EXISTS stats (
          id INTEGER PRIMARY KEY,
          timestamp TEXT NOT NULL,
          metric_name TEXT NOT NULL,
          metric_value REAL NOT NULL,
          metadata TEXT
        );`
      )
      .run();
    this.db
      .query(
        `CREATE INDEX IF NOT EXISTS idx_metric_time
         ON stats(metric_name, timestamp);`
      )
      .run();
  }
}
