import { readdir, readFile } from "node:fs/promises";
import path from "node:path";
import type { ExampleDescriptor, ExampleFileDescriptor } from "../shared/types";

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
      const files = await collectRaeFiles(fullPath, relativePath);
      if (!files.length) continue;
      const entryFile =
        files.find((file) => file.path.endsWith("main.rae"))?.path ?? files[0].path;
      examples.push(makeMultiFileExample(relativePath, entryFile, files));
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
  files: ExampleFileDescriptor[]
): ExampleDescriptor {
  return {
    id,
    name: id,
    entry,
    files
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

function sanitizePath(root: string, relativePath: string): string {
  const resolvedRoot = path.resolve(root);
  const resolved = path.resolve(resolvedRoot, relativePath);
  if (!resolved.startsWith(resolvedRoot)) {
    throw new Error("Invalid example path");
  }
  return resolved;
}
