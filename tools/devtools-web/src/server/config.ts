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

export type ExampleCategoryConfig = {
  id: string;
  label: string;
  min: number;
  max: number;
};

export type RaeDevtoolsConfig = {
  compilerPath: string;
  port: number;
  testsPath?: string;
  syntaxSummaryPath?: string;
  examplesPath?: string;
  targets: TargetConfig[];
  defaultTarget: string;
  exampleCategories?: ExampleCategoryConfig[];
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
    exampleRunCommand: "./compiler/bin/rae run --project {{ENTRY_DIR}} {{ENTRY}}",
    exampleWatchCommand: "./compiler/bin/rae run --project {{ENTRY_DIR}} --watch {{ENTRY}}",
    exampleBuildCommand: "./compiler/bin/rae build --target live --project {{ENTRY_DIR}} --out {{OUTDIR}} {{ENTRY}}"
  },
  {
    id: "compiled",
    label: "Compiled (C backend)",
    description: "Generates C code for native builds",
    testCommand: "cd compiler && TEST_TARGET=compiled make test",
    buildCommand: "cd compiler && make",
    cleanCommand: "cd compiler && make clean",
    rebuildCommand: "cd compiler && make clean && make",
    // Link raylib STATICALLY (libraylib.a, not -lraylib) so the
    // bundled GLFW symbols (glfwWaitEventsTimeout etc., used by the
    // event-driven UI loop in lib/ui/event_loop.rae) are present in
    // the final binary. The shared library libraylib.dylib does NOT
    // export those symbols. Same fix as compiler/Makefile and
    // compiler/src/main.c.
    exampleRunCommand: "./compiler/bin/rae build --target compiled --project {{ENTRY_DIR}} --emit-c --out {{OUTDIR}}/out.c {{ENTRY}} && gcc -O2 -o {{OUTDIR}}/app {{OUTDIR}}/out.c {{OUTDIR}}/rae_runtime.c third_party/raylib/rae_raylib.c -I{{OUTDIR}} -Ithird_party/raylib -I/opt/homebrew/include /opt/homebrew/lib/libraylib.a -framework CoreVideo -framework IOKit -framework Cocoa -framework OpenGL && {{OUTDIR}}/app",
    exampleBuildCommand: "./compiler/bin/rae build --target compiled --project {{ENTRY_DIR}} --emit-c --out {{OUTDIR}} {{ENTRY}}"
  },
  {
    id: "hybrid",
    label: "Hybrid Dev",
    description: "Compiled host plus Live bundle packaging",
    testCommand: "cd compiler && TEST_TARGET=hybrid make test",
    buildCommand: "cd compiler && make",
    cleanCommand: "cd compiler && make clean",
    rebuildCommand: "cd compiler && make clean && make",
    exampleRunCommand: "./compiler/bin/rae build --target hybrid --project {{ENTRY_DIR}} --emit-c --out {{OUTDIR}} {{ENTRY}}",
    exampleBuildCommand: "./compiler/bin/rae build --target hybrid --project {{ENTRY_DIR}} --emit-c --out {{OUTDIR}} {{ENTRY}}"
  }
];

const DEFAULT_COMPILER_TARGET_IDS = ["live", "compiled"];

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
const LOCAL_CONFIG_PATH = path.join(PROJECT_ROOT, "config.local.json");

export async function loadConfig(customPath?: string): Promise<RaeDevtoolsConfig> {
  const filePath = customPath ?? CONFIG_PATH;
  const baseConfig = await readConfigFile(filePath);
  const localConfig = await readConfigFile(LOCAL_CONFIG_PATH);

  if (!baseConfig) {
    console.warn(
      `[config] Missing config at ${filePath}. Using defaults${
        localConfig ? " with config.local.json overrides" : ""
      }.`
    );
  }

  let merged: PartialConfig = baseConfig ?? {};
  if (localConfig) {
    merged = mergePartialConfigs(merged, localConfig);
  }

  const config = normalizeConfig(merged);
  const sourceParts = [
    baseConfig ? path.basename(filePath) : "defaults",
    localConfig ? path.basename(LOCAL_CONFIG_PATH) : null
  ].filter((part): part is string => Boolean(part));
  config.configSource = sourceParts.join(" + ");
  logConfigSummary(config);
  return config;
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

async function readConfigFile(filePath: string): Promise<PartialConfig | null> {
  try {
    const contents = await readFile(filePath, "utf8");
    const parsed = JSON.parse(contents) as PartialConfig;
    return parsed ?? {};
  } catch (error) {
    const nodeError = error as NodeJS.ErrnoException;
    if (nodeError.code === "ENOENT") {
      return null;
    }
    console.error(`[config] Failed to parse config at ${filePath}`);
    throw error;
  }
}

function mergePartialConfigs(base: PartialConfig, override: PartialConfig): PartialConfig {
  return {
    ...base,
    ...override,
    targets: override.targets ?? base.targets,
    defaultTarget: override.defaultTarget ?? base.defaultTarget
  };
}

function normalizeConfig(parsed: PartialConfig = {}): RaeDevtoolsConfig {
  const base: RaeDevtoolsConfig = {
    compilerPath: parsed.compilerPath ?? DEFAULT_CONFIG.compilerPath,
    port: parsed.port ?? DEFAULT_CONFIG.port,
    testsPath: parsed.testsPath ?? DEFAULT_CONFIG.testsPath,
    syntaxSummaryPath: parsed.syntaxSummaryPath ?? DEFAULT_CONFIG.syntaxSummaryPath,
    examplesPath: parsed.examplesPath ?? DEFAULT_CONFIG.examplesPath,
    targets: [],
    defaultTarget: parsed.defaultTarget ?? DEFAULT_CONFIG.defaultTarget,
    exampleCategories: parsed.exampleCategories
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

export function getTestHistoryPath(config: RaeDevtoolsConfig): string {
  return resolveCompilerPath(config, "compiler/stats/test_history.json");
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

export function resolveTargetList(
  config: RaeDevtoolsConfig,
  targetIds?: string[]
): TargetConfig[] {
  const resolved: TargetConfig[] = [];
  const appendTarget = (targetId: string) => {
    const match = config.targets.find((target) => target.id === targetId);
    if (match && !resolved.includes(match)) {
      resolved.push(match);
    }
  };

  if (Array.isArray(targetIds) && targetIds.length) {
    targetIds.forEach(appendTarget);
    return resolved.length ? resolved : [resolveTarget(config)];
  }

  DEFAULT_COMPILER_TARGET_IDS.forEach(appendTarget);
  return resolved.length ? resolved : [...config.targets];
}
