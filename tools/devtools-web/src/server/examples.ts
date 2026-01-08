import { spawn } from "node:child_process";
import { readdir, readFile, stat, writeFile } from "node:fs/promises";
import { createHash } from "node:crypto";
import path from "node:path";
import type {
  ExampleActionDescriptor,
  ExampleDescriptor,
  ExampleFileDescriptor
} from "../shared/types";

type ExampleMetadata = {
  name?: string;
  entry?: string;
  description?: string;
  supportedTargets?: string[];
  defaultTargetId?: string;
  actions?: ExampleActionMetadata[];
};

type ExampleActionMetadata = {
  id?: string;
  label?: string;
  description?: string;
  command?: string;
  targetId?: string;
};

type RaePackSource = {
  path: string;
  emit: string;
};

type RaePackTarget = {
  id: string;
  label: string;
  entry: string;
  sources: RaePackSource[];
};

type RaePackJson = {
  name: string;
  format: string;
  version: number;
  defaultTarget: string;
  targets: RaePackTarget[];
};

type ExamplePackInfo = {
  entry?: string;
  supportedTargets?: string[];
  defaultTargetId?: string;
  targetEntries?: Record<string, string>;
};

const ACTION_REGISTRY = new Map<string, ExampleActionMetadata[]>();
type ExampleDownloads = Array<{
  profile: string;
  version: string;
  builds: Array<{
    name: string;
    path: string;
    files: Array<{ name: string; size: number; hash: string }>;
  }>;
}>;

const EXAMPLE_ORDER: Record<string, number> = {
  hello: 1,
  hello_compiled: 2,
  math: 3,
  call_stack: 4,
  hot_reload: 5,
  auto_import_demo: 6,
  multifile_report: 7,
  external_c_library: 8,
  hybrid_hot_reload: 9,
  raylib_basic: 12,
  pong: 13,
  raepack_demo: 14
};

export async function listExamples(
  root: string,
  compilerBinPath: string
): Promise<ExampleDescriptor[]> {
  const entries = await safeReadDir(root);
  const examples: ExampleDescriptor[] = [];
  ACTION_REGISTRY.clear();

  for (const entry of entries) {
    if (entry.name.startsWith(".")) continue;
    const relativePath = entry.name;
    const fullPath = path.join(root, entry.name);

    if (entry.isFile() && entry.name.endsWith(".rae")) {
      examples.push(makeSingleFileExample(relativePath));
      continue;
    }

    if (entry.isDirectory()) {
      const metadata = await readExampleMetadata(fullPath);
      const files = await collectExampleFiles(fullPath, relativePath);
      if (!files.length) continue;
      const packInfo = await readExamplePack(fullPath, relativePath, compilerBinPath);
      const entryFile =
        packInfo?.entry ?? resolveEntryFile(files, metadata?.entry, relativePath);
      const normalizedActions = normalizeExampleActions(relativePath, metadata?.actions);
      if (normalizedActions.length) {
        ACTION_REGISTRY.set(relativePath, normalizedActions);
      }
      const descriptor = makeMultiFileExample(
        relativePath,
        entryFile,
        files,
        metadata,
        packInfo
      );
      if (normalizedActions.length) {
        descriptor.actions = normalizedActions.map((action) => ({
          id: action.id!,
          label: action.label ?? action.id!,
          description: action.description,
          targetId: action.targetId
        }));
      }
      examples.push(descriptor);
    }
  }

  return applyExampleOrdering(examples);
}

function makeSingleFileExample(relativePath: string): ExampleDescriptor {
  return {
    id: relativePath.replace(/\.rae$/, ""),
    name: relativePath.replace(/\.rae$/, ""),
    entry: relativePath,
    files: [{ path: relativePath, name: relativePath }] satisfies ExampleFileDescriptor[]
  };
}

function makeMultiFileExample(
  id: string,
  entry: string,
  files: ExampleFileDescriptor[],
  metadata: ExampleMetadata | null,
  packInfo: ExamplePackInfo | null
): ExampleDescriptor {
  const supportedTargets = packInfo?.supportedTargets
    ?? (Array.isArray(metadata?.supportedTargets)
      ? metadata!.supportedTargets.filter((value): value is string => typeof value === "string")
      : undefined);
  const defaultTargetId =
    packInfo?.defaultTargetId
    ?? (typeof metadata?.defaultTargetId === "string" ? metadata.defaultTargetId : undefined);
  const descriptor: ExampleDescriptor = {
    id,
    name: metadata?.name ?? id,
    entry,
    files,
    description: metadata?.description,
    supportedTargets: supportedTargets?.length ? supportedTargets : undefined,
    defaultTargetId,
    targetEntries: packInfo?.targetEntries
  };
  return descriptor;
}

function applyExampleOrdering(examples: ExampleDescriptor[]): ExampleDescriptor[] {
  const orders = Object.values(EXAMPLE_ORDER);
  let nextOrder = orders.length ? Math.max(...orders) + 1 : 1;
  const decorated = examples.map((example) => {
    const order = EXAMPLE_ORDER[example.id] ?? nextOrder++;
    const baseName = stripOrderPrefix(example.name ?? example.id);
    const numberedName = `${order}. ${baseName}`;
    return { example: { ...example, name: numberedName }, order, name: numberedName };
  });
  decorated.sort((a, b) => {
    if (a.order !== b.order) {
      return a.order - b.order;
    }
    return a.name.localeCompare(b.name);
  });
  return decorated.map((item) => item.example);
}

function stripOrderPrefix(name: string): string {
  return name.replace(/^\d+\.\s+/, "");
}

async function collectExampleFiles(
  root: string,
  relativeBase: string
): Promise<ExampleFileDescriptor[]> {
  const entries = await safeReadDir(root);
  const files: ExampleFileDescriptor[] = [];

  const ALLOWED_EXTENSIONS = [".rae", ".md", ".raepack", ".json", ".txt"];

  for (const entry of entries) {
    if (entry.name.startsWith(".")) continue;
    if (entry.name === "devtools.json") continue;
    
    const relativePath = path.join(relativeBase, entry.name);
    const fullPath = path.join(root, entry.name);

    if (entry.isDirectory()) {
      const nested = await collectExampleFiles(fullPath, relativePath);
      files.push(...nested);
      continue;
    }

    if (entry.isFile()) {
      const ext = path.extname(entry.name).toLowerCase();
      if (ALLOWED_EXTENSIONS.includes(ext)) {
        files.push({ path: relativePath, name: entry.name });
      }
    }
  }

  return files.sort((a, b) => a.path.localeCompare(b.path));
}

async function safeReadDir(dir: string) {
  try {
    return await readdir(dir, { withFileTypes: true });
  } catch (error) {
    console.warn(`[examples] Failed to read directory ${dir}`, error);
    return [];
  }
}

export async function readExampleFile(root: string, relativePath: string): Promise<string> {
  const safePath = sanitizePath(root, relativePath);
  return readFile(safePath, "utf8");
}

export async function writeExampleFile(root: string, relativePath: string, contents: string) {
  const safePath = sanitizePath(root, relativePath);
  await writeFile(safePath, contents, "utf8");
}

function sanitizePath(root: string, relativePath: string): string {
  const resolvedRoot = path.resolve(root);
  const resolved = path.resolve(resolvedRoot, relativePath);
  if (!resolved.startsWith(resolvedRoot)) {
    throw new Error("Invalid example path");
  }
  return resolved;
}

async function readExampleMetadata(dir: string): Promise<ExampleMetadata | null> {
  const metadataPath = path.join(dir, "devtools.json");
  try {
    const contents = await readFile(metadataPath, "utf8");
    const parsed = JSON.parse(contents);
    if (parsed && typeof parsed === "object") {
      return parsed as ExampleMetadata;
    }
    return null;
  } catch (error) {
    const nodeError = error as NodeJS.ErrnoException;
    if (nodeError.code !== "ENOENT") {
      console.warn(`[examples] Failed to read metadata for ${dir}`, error);
    }
    return null;
  }
}

function resolveEntryFile(
  files: ExampleFileDescriptor[],
  metadataEntry: string | undefined,
  relativeBase: string
) {
  if (typeof metadataEntry === "string" && metadataEntry.trim().length > 0) {
    const normalized = path
      .join(relativeBase, metadataEntry)
      .split(path.sep)
      .join(path.posix.sep);
    const match = files.find((file) => file.path === normalized);
    if (match) return match.path;
    console.warn(
      `[examples] Metadata entry "${metadataEntry}" not found for ${relativeBase}, falling back to default entry.`
    );
  }
  return files.find((file) => file.path.endsWith("main.rae"))?.path ?? files[0].path;
}

async function readExamplePack(
  dir: string,
  exampleId: string,
  compilerBinPath: string
): Promise<ExamplePackInfo | null> {
  const packPath = await findRaePackFile(dir);
  if (!packPath) return null;
  const pack = await readRaePack(compilerBinPath, packPath);
  if (!pack || !Array.isArray(pack.targets)) return null;

  const targetEntries: Record<string, string> = {};
  const supportedTargets: string[] = [];
  pack.targets.forEach((target) => {
    if (!target || typeof target.id !== "string" || typeof target.entry !== "string") {
      return;
    }
    const entry = normalizePackEntry(target.entry);
    const entryPath = path.posix.join(exampleId, entry.replace(/^\/+/, ""));
    targetEntries[target.id] = entryPath;
    supportedTargets.push(target.id);
  });
  const defaultTargetId =
    typeof pack.defaultTarget === "string" && targetEntries[pack.defaultTarget]
      ? pack.defaultTarget
      : undefined;
  const entry =
    (defaultTargetId && targetEntries[defaultTargetId]) ||
    targetEntries[supportedTargets[0] ?? ""] ||
    undefined;

  return {
    entry,
    supportedTargets: supportedTargets.length ? supportedTargets : undefined,
    defaultTargetId,
    targetEntries: Object.keys(targetEntries).length ? targetEntries : undefined
  };
}

async function findRaePackFile(dir: string): Promise<string | null> {
  const entries = await safeReadDir(dir);
  const packs = entries
    .filter((entry) => entry.isFile() && entry.name.endsWith(".raepack"))
    .map((entry) => entry.name)
    .sort();
  if (!packs.length) {
    return null;
  }
  if (packs.length > 1) {
    console.warn(`[examples] Multiple .raepack files found in ${dir}. Using ${packs[0]}.`);
  }
  return path.join(dir, packs[0]);
}

async function readRaePack(
  compilerBinPath: string,
  packPath: string
): Promise<RaePackJson | null> {
  try {
    await stat(compilerBinPath);
  } catch {
    console.warn(`[examples] Compiler binary missing at ${compilerBinPath}.`);
    return null;
  }

  return await new Promise((resolve) => {
    const child = spawn(compilerBinPath, ["pack", "--json", packPath], {
      cwd: path.dirname(compilerBinPath),
      env: process.env
    });
    let stdout = "";
    let stderr = "";
    child.stdout?.setEncoding("utf8");
    child.stderr?.setEncoding("utf8");
    child.stdout?.on("data", (chunk: string) => {
      stdout += chunk;
    });
    child.stderr?.on("data", (chunk: string) => {
      stderr += chunk;
    });
    child.on("error", (error) => {
      console.warn(`[examples] Failed to run rae pack: ${error}`);
      resolve(null);
    });
    child.on("close", (code) => {
      if (code !== 0) {
        const message = stderr.trim() || `rae pack exited with ${code}`;
        console.warn(`[examples] Failed to parse ${packPath}: ${message}`);
        resolve(null);
        return;
      }
      try {
        const parsed = JSON.parse(stdout) as RaePackJson;
        resolve(parsed);
      } catch (error) {
        console.warn(`[examples] Invalid raepack JSON for ${packPath}`, error);
        resolve(null);
      }
    });
  });
}

function normalizePackEntry(entry: string): string {
  const normalized = entry.replace(/\\/g, "/");
  const cleaned = normalized.replace(/^\.\//, "");
  return path.posix.normalize(cleaned);
}

function normalizeExampleActions(
  exampleId: string,
  actions: ExampleActionMetadata[] | undefined
): ExampleActionMetadata[] {
  if (!Array.isArray(actions)) return [];
  const normalized: ExampleActionMetadata[] = [];
  actions.forEach((action, index) => {
    if (!action || typeof action !== "object") {
      console.warn(`[examples] Invalid action entry in ${exampleId} metadata at index ${index}.`);
      return;
    }
    if (!action.id || typeof action.id !== "string") {
      console.warn(
        `[examples] Action entry at index ${index} in ${exampleId} metadata is missing an id.`
      );
      return;
    }
    if (!action.command || typeof action.command !== "string") {
      console.warn(
        `[examples] Action "${action.id}" in ${exampleId} metadata missing command. Skipping.`
      );
      return;
    }
    normalized.push({
      id: action.id,
      label: action.label ?? action.id,
      description: action.description,
      command: action.command,
      targetId: action.targetId
    });
  });
  return normalized;
}

export function resolveExampleAction(exampleId: string, actionId: string) {
  const actions = ACTION_REGISTRY.get(exampleId);
  if (!actions) return null;
  return actions.find((action) => action.id === actionId) ?? null;
}

export async function listSimulatedDownloads(
  examplesRoot: string,
  exampleId: string
): Promise<ExampleDownloads> {
  if (!exampleId) return [];
  const baseDir = path.resolve(examplesRoot, exampleId, ".simulated_downloads");
  let profiles = [];
  try {
    profiles = await readdir(baseDir, { withFileTypes: true });
  } catch (error) {
    const nodeError = error as NodeJS.ErrnoException;
    if (nodeError.code === "ENOENT") {
      return [];
    }
    console.warn(`[examples] Failed to read simulated downloads for ${exampleId}`, error);
    return [];
  }
  const downloads: ExampleDownloads = [];
  for (const profile of profiles) {
    if (!profile.isDirectory()) continue;
    const profileDir = path.join(baseDir, profile.name);
    const entries = await safeReadDir(profileDir);
    const versionedBuilds = new Map<string, Array<{ name: string; path: string; files: Array<{ name: string; size: number; hash: string }> }>>();
    const legacyBuilds: Array<{ name: string; path: string; files: Array<{ name: string; size: number; hash: string }> }> = [];

    for (const entry of entries) {
      if (!entry.isDirectory()) continue;
      const entryPath = path.join(profileDir, entry.name);
      const children = await safeReadDir(entryPath);
      const hasFiles = children.some((child) => child.isFile());
      const hasDirs = children.some((child) => child.isDirectory());

      if (hasFiles) {
        const files = await collectDownloadFiles(entryPath);
        legacyBuilds.push({
          name: entry.name,
          path: path.relative(baseDir, entryPath).split(path.sep).join(path.posix.sep),
          files
        });
        continue;
      }

      if (hasDirs) {
        const builds = [];
        for (const child of children) {
          if (!child.isDirectory()) continue;
          const buildPath = path.join(entryPath, child.name);
          const files = await collectDownloadFiles(buildPath);
          builds.push({
            name: child.name,
            path: path.relative(baseDir, buildPath).split(path.sep).join(path.posix.sep),
            files
          });
        }
        builds.sort((a, b) => b.name.localeCompare(a.name));
        versionedBuilds.set(entry.name, builds);
      }
    }

    if (legacyBuilds.length) {
      legacyBuilds.sort((a, b) => b.name.localeCompare(a.name));
      downloads.push({ profile: profile.name, version: "default", builds: legacyBuilds });
    }

    for (const [version, builds] of versionedBuilds) {
      downloads.push({ profile: profile.name, version, builds });
    }
  }

  return downloads.sort((a, b) => {
    if (a.profile !== b.profile) {
      return a.profile.localeCompare(b.profile);
    }
    return a.version.localeCompare(b.version);
  });
}

async function collectDownloadFiles(dir: string) {
  const entries = await safeReadDir(dir);
  const result: Array<{ name: string; size: number; hash: string }> = [];
  for (const entry of entries) {
    if (!entry.isFile()) continue;
    const filePath = path.join(dir, entry.name);
    try {
      const buffer = await readFile(filePath);
      const hash = createHash("sha256").update(buffer).digest("hex");
      result.push({
        name: entry.name,
        size: buffer.length,
        hash
      });
    } catch (error) {
      console.warn(`[examples] Failed to read download artifact ${filePath}`, error);
    }
  }
  return result.sort((a, b) => a.name.localeCompare(b.name));
}
