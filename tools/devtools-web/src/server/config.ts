import { readFile } from "node:fs/promises";
import path from "node:path";

export type RaeDevtoolsConfig = {
  compilerPath: string;
  buildCommand: string;
  testCommand: string;
  rebuildCommand?: string;
  cleanCommand: string;
  port: number;
  testsPath?: string;
  syntaxSummaryPath?: string;
  examplesPath?: string;
};

const DEFAULT_CONFIG: RaeDevtoolsConfig = {
  compilerPath: "../rae",
  buildCommand: "cd compiler && make",
  testCommand: "cd compiler && make test",
  rebuildCommand: "cd compiler && make clean && make",
  cleanCommand: "cd compiler && make clean",
  port: 3000,
  testsPath: "compiler/tests",
  syntaxSummaryPath: "docs/rae_syntax.json",
  examplesPath: "examples"
};

const PROJECT_ROOT = path.resolve(process.cwd());
const CONFIG_PATH = path.join(PROJECT_ROOT, "config.json");

export async function loadConfig(customPath?: string): Promise<RaeDevtoolsConfig> {
  const filePath = customPath ?? CONFIG_PATH;

  try {
    const contents = await readFile(filePath, "utf8");
    const parsed = JSON.parse(contents) as Partial<RaeDevtoolsConfig>;
    return {
      ...DEFAULT_CONFIG,
      ...parsed
    };
  } catch (error) {
    const nodeError = error as NodeJS.ErrnoException;
    if (nodeError.code === "ENOENT") {
      console.warn(
        `[config] Missing config.json at ${filePath}. Falling back to defaults (copy config.example.json to configure).`
      );
      return DEFAULT_CONFIG;
    }

    console.error(`[config] Failed to parse config at ${filePath}`);
    throw error;
  }
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
