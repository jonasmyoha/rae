import { readFile } from "node:fs/promises";
import path from "node:path";

export type TargetConfig = {
  id: string;
  label: string;
  description?: string;
  testCommand?: string;
  buildCommand?: string;
  cleanCommand?: string;
  rebuildCommand?: string;
  exampleRunCommand?: string;
  exampleWatchCommand?: string;
  exampleBuildCommand?: string;
};

export type RaeDevtoolsConfig = {
  compilerPath: string;
  port: number;
  testsPath?: string;
  syntaxSummaryPath?: string;
  examplesPath?: string;
  targets: TargetConfig[];
  defaultTarget: string;
  configSource?: string;
};

type LegacyCommandConfig = {
  buildCommand?: string;
  testCommand?: string;
  cleanCommand?: string;
  rebuildCommand?: string;
};

type PartialConfig = Partial<Omit<RaeDevtoolsConfig, "targets" | "defaultTarget">> &
  LegacyCommandConfig & { targets?: TargetConfig[]; defaultTarget?: string };

const DEFAULT_TARGETS: TargetConfig[] = [
  {
    id: "live",
    label: "Live (bytecode VM)",
    description: "Hot-reload friendly bytecode VM build",
    testCommand: "cd compiler && TEST_TARGET=live make test",
    buildCommand: "cd compiler && make",
    cleanCommand: "cd compiler && make clean",
    rebuildCommand: "cd compiler && make clean && make",
    exampleRunCommand: "./compiler/bin/rae run {{ENTRY}}",
    exampleWatchCommand: "./compiler/bin/rae run --watch {{ENTRY}}",
    exampleBuildCommand: "./compiler/bin/rae build --target live --out {{OUTDIR}} {{ENTRY}}"
  },
  {
    id: "compiled",
    label: "Compiled (C backend)",
    description: "Generates C code for native builds",
    testCommand: "cd compiler && TEST_TARGET=compiled make test",
    buildCommand: "cd compiler && make",
    cleanCommand: "cd compiler && make clean",
    rebuildCommand: "cd compiler && make clean && make",
    exampleRunCommand: "./compiler/bin/rae build --target compiled --out {{OUTDIR}} {{ENTRY}}",
    exampleBuildCommand: "./compiler/bin/rae build --target compiled --out {{OUTDIR}} {{ENTRY}}"
  },
  {
    id: "hybrid",
    label: "Hybrid Dev",
    description: "Compiled host plus Live bundle packaging",
    testCommand: "cd compiler && TEST_TARGET=hybrid make test",
    buildCommand: "cd compiler && make",
    cleanCommand: "cd compiler && make clean",
    rebuildCommand: "cd compiler && make clean && make",
    exampleRunCommand: "./compiler/bin/rae build --target hybrid --out {{OUTDIR}} {{ENTRY}}",
    exampleBuildCommand: "./compiler/bin/rae build --target hybrid --out {{OUTDIR}} {{ENTRY}}"
  }
];

const DEFAULT_CONFIG: RaeDevtoolsConfig = {
  compilerPath: "../rae",
  port: 3000,
  testsPath: "compiler/tests",
  syntaxSummaryPath: "docs/rae_syntax.json",
  examplesPath: "examples",
  targets: DEFAULT_TARGETS,
  defaultTarget: DEFAULT_TARGETS[0]!.id
};

const PROJECT_ROOT = path.resolve(process.cwd());
const CONFIG_PATH = path.join(PROJECT_ROOT, "config.json");

export async function loadConfig(customPath?: string): Promise<RaeDevtoolsConfig> {
  const filePath = customPath ?? CONFIG_PATH;

  try {
    const contents = await readFile(filePath, "utf8");
    const parsed = JSON.parse(contents) as PartialConfig;
    const config = normalizeConfig(parsed);
    config.configSource = filePath;
    logConfigSummary(config);
    return config;
  } catch (error) {
    const nodeError = error as NodeJS.ErrnoException;
    if (nodeError.code === "ENOENT") {
      console.warn(
        `[config] Missing config.json at ${filePath}. Falling back to defaults (copy config.example.json to configure).`
      );
      const config = { ...DEFAULT_CONFIG, configSource: "defaults" };
      logConfigSummary(config);
      return config;
    }

    console.error(`[config] Failed to parse config at ${filePath}`);
    throw error;
  }
}

function logConfigSummary(config: RaeDevtoolsConfig) {
  const source = config.configSource ?? "unknown";
  console.log(`[config] Loaded config (${source})`);
  console.log(`[config] CWD: ${process.cwd()}`);
  console.log(`[config] Compiler path: ${config.compilerPath}`);
  config.targets.forEach((target) => {
    console.log(
      `[config] Target ${target.id}: test="${target.testCommand ?? "-"}" build="${target.buildCommand ?? "-"}"`
    );
  });
}

function normalizeConfig(parsed: PartialConfig = {}): RaeDevtoolsConfig {
  const base: RaeDevtoolsConfig = {
    compilerPath: parsed.compilerPath ?? DEFAULT_CONFIG.compilerPath,
    port: parsed.port ?? DEFAULT_CONFIG.port,
    testsPath: parsed.testsPath ?? DEFAULT_CONFIG.testsPath,
    syntaxSummaryPath: parsed.syntaxSummaryPath ?? DEFAULT_CONFIG.syntaxSummaryPath,
    examplesPath: parsed.examplesPath ?? DEFAULT_CONFIG.examplesPath,
    targets: [],
    defaultTarget: parsed.defaultTarget ?? DEFAULT_CONFIG.defaultTarget
  };

  const resolvedTargets = deriveTargets(parsed);
  const defaultTarget = resolvedTargets.find((target) => target.id === base.defaultTarget)?.id
    ?? resolvedTargets[0]?.id
    ?? DEFAULT_CONFIG.defaultTarget;

  return {
    ...base,
    targets: resolvedTargets.length ? resolvedTargets : DEFAULT_TARGETS,
    defaultTarget
  };
}

function deriveTargets(parsed: PartialConfig): TargetConfig[] {
  if (Array.isArray(parsed.targets) && parsed.targets.length) {
    return parsed.targets
      .map((target, index) => normalizeTarget(target, index))
      .filter((target): target is TargetConfig => Boolean(target));
  }

  const legacyTarget = buildLegacyTarget(parsed);
  return legacyTarget ? [legacyTarget] : [];
}

function normalizeTarget(target: TargetConfig | undefined, index: number): TargetConfig | null {
  if (!target || !target.id) {
    console.warn(
      `[config] Ignoring target entry at index ${index} because it is missing an id property.`
    );
    return null;
  }
  return {
    ...target,
    label: target.label || target.id
  };
}

function buildLegacyTarget(parsed: LegacyCommandConfig): TargetConfig | null {
  if (
    !parsed.buildCommand &&
    !parsed.cleanCommand &&
    !parsed.rebuildCommand &&
    !parsed.testCommand
  ) {
    return null;
  }

  return {
    ...DEFAULT_TARGETS[0],
    id: "live",
    label: "Live (bytecode VM)",
    testCommand: parsed.testCommand ?? DEFAULT_TARGETS[0]!.testCommand,
    buildCommand: parsed.buildCommand ?? DEFAULT_TARGETS[0]!.buildCommand,
    cleanCommand: parsed.cleanCommand ?? DEFAULT_TARGETS[0]!.cleanCommand,
    rebuildCommand: parsed.rebuildCommand ?? DEFAULT_TARGETS[0]!.rebuildCommand
  };
}

export function getProjectRoot(): string {
  return PROJECT_ROOT;
}

export function resolveCompilerPath(config: RaeDevtoolsConfig, ...segments: string[]): string {
  return path.resolve(process.cwd(), config.compilerPath, ...segments);
}

export function getTestsRoot(config: RaeDevtoolsConfig): string {
  return resolveCompilerPath(config, config.testsPath ?? "tests");
}

export function getSyntaxSummaryPath(config: RaeDevtoolsConfig): string {
  return resolveCompilerPath(config, config.syntaxSummaryPath ?? "docs/rae_syntax.json");
}

export function getExamplesRoot(config: RaeDevtoolsConfig): string {
  return resolveCompilerPath(config, config.examplesPath ?? "examples");
}

export function resolveTarget(config: RaeDevtoolsConfig, targetId?: string): TargetConfig {
  if (targetId) {
    const match = config.targets.find((target) => target.id === targetId);
    if (match) return match;
    console.warn(`[config] Requested target "${targetId}" not found. Falling back to default.`);
  }
  return (
    config.targets.find((target) => target.id === config.defaultTarget) ?? config.targets[0]!
  );
}
