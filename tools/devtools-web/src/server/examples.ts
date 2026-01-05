import { readdir, readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import type { ExampleDescriptor, ExampleFileDescriptor } from "../shared/types";

type ExampleMetadata = {
  name?: string;
  entry?: string;
  description?: string;
  supportedTargets?: string[];
  defaultTargetId?: string;
};

export async function listExamples(root: string): Promise<ExampleDescriptor[]> {
  const entries = await safeReadDir(root);
  const examples: ExampleDescriptor[] = [];

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
      examples.push(
        makeMultiFileExample(relativePath, entryFile, files, metadata)
      );
    }
  }

  return examples.sort((a, b) => a.name.localeCompare(b.name));
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
  return {
    id,
    name: metadata?.name ?? id,
    entry,
    files,
    description: metadata?.description,
    supportedTargets: supportedTargets?.length ? supportedTargets : undefined,
    defaultTargetId: typeof metadata?.defaultTargetId === "string" ? metadata.defaultTargetId : undefined
  };
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
