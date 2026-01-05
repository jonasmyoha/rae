import { readdir, readFile, writeFile } from "node:fs/promises";
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
  main_loop: 2,
  hot_reload: 3,
  vm_call: 4,
  vm_math: 5,
  c_backend_demo: 6,
  auto_import_demo: 7,
  multifile_report: 8,
  "tinyexpr-demo": 9,
  hybrid_hot_reload: 10
};

export async function listExamples(root: string): Promise<ExampleDescriptor[]> {
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
      const files = await collectRaeFiles(fullPath, relativePath);
      if (!files.length) continue;
      const entryFile = resolveEntryFile(files, metadata?.entry, relativePath);
      const normalizedActions = normalizeExampleActions(relativePath, metadata?.actions);
      if (normalizedActions.length) {
        ACTION_REGISTRY.set(relativePath, normalizedActions);
      }
      const descriptor = makeMultiFileExample(relativePath, entryFile, files, metadata);
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
  metadata: ExampleMetadata | null
): ExampleDescriptor {
  const supportedTargets = Array.isArray(metadata?.supportedTargets)
    ? metadata!.supportedTargets.filter((value): value is string => typeof value === "string")
    : undefined;
  const descriptor: ExampleDescriptor = {
    id,
    name: metadata?.name ?? id,
    entry,
    files,
    description: metadata?.description,
    supportedTargets: supportedTargets?.length ? supportedTargets : undefined,
    defaultTargetId: typeof metadata?.defaultTargetId === "string" ? metadata.defaultTargetId : undefined
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

async function collectRaeFiles(
  root: string,
  relativeBase: string
): Promise<ExampleFileDescriptor[]> {
  const entries = await safeReadDir(root);
  const files: ExampleFileDescriptor[] = [];

  for (const entry of entries) {
    if (entry.name.startsWith(".")) continue;
    const relativePath = path.join(relativeBase, entry.name);
    const fullPath = path.join(root, entry.name);

    if (entry.isDirectory()) {
      const nested = await collectRaeFiles(fullPath, relativePath);
      files.push(...nested);
      continue;
    }

    if (entry.isFile() && entry.name.endsWith(".rae")) {
      files.push({ path: relativePath, name: entry.name });
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
