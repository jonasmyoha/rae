import { readFile } from "node:fs/promises";
import path from "node:path";

export type RaeDevtoolsConfig = {
  compilerPath: string;
  buildCommand: string;
  testCommand: string;
  cleanCommand: string;
  port: number;
};

const DEFAULT_CONFIG: RaeDevtoolsConfig = {
  compilerPath: "../rae",
  buildCommand: "make",
  testCommand: "make test",
  cleanCommand: "make clean",
  port: 3000
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
