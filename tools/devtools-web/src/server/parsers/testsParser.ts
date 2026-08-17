export type ParsedTestEvent =
  | { type: "test-pass"; name: string }
  | { type: "test-fail"; name: string; details?: string }
  | { type: "test-error"; name: string; details?: string }
  | { type: "summary"; passed: number; failed: number };

const PASS_REGEX = /^PASS:\s+(?<name>\S+)/i;
const FAIL_REGEX = /^(FAIL|ERROR):\s+(?<name>\S+)(?<details>.*)?/i;
const SUMMARY_REGEX = /^Results:\s+(?<passed>\d+)\s+passed,\s+(?<failed>\d+)\s+failed/i;

export function parseTestLine(line: string): ParsedTestEvent | null {
  const passMatch = line.match(PASS_REGEX);
  if (passMatch?.groups?.name) {
    return { type: "test-pass", name: passMatch.groups.name };
  }

  const failMatch = line.match(FAIL_REGEX);
  if (failMatch?.groups?.name) {
    const type = line.startsWith("ERROR") ? "test-error" : "test-fail";
    const details = failMatch.groups.details?.trim();
    return { type, name: failMatch.groups.name, details };
  }

  const summaryMatch = line.match(SUMMARY_REGEX);
  if (summaryMatch?.groups) {
    return {
      type: "summary",
      passed: Number(summaryMatch.groups.passed),
      failed: Number(summaryMatch.groups.failed)
    };
  }

  return null;
}
