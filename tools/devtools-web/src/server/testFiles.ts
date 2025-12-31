import { readdir, readFile } from "node:fs/promises";
import path from "node:path";

export type TestFileNode = {
  name: string;
  path: string;
  type: "file" | "directory";
  children?: TestFileNode[];
};

export async function readTestTree(
  root: string,
  maxDepth = 3,
  relativeBase = ""
): Promise<TestFileNode[]> {
  const currentDir = relativeBase ? path.join(root, relativeBase) : root;
  const entries = await safeReadDir(currentDir);
  const nodes: TestFileNode[] = [];
  for (const entry of entries) {
    if (entry.name.startsWith(".") || entry.name.startsWith("_")) continue;
    const relativePath = relativeBase ? path.join(relativeBase, entry.name) : entry.name;
    if (entry.isDirectory()) {
      const children =
        maxDepth > 1 ? await readTestTree(root, maxDepth - 1, relativePath) : undefined;
      nodes.push({
        name: entry.name,
        path: relativePath,
        type: "directory",
        children
      });
    } else if (entry.isFile() && isTestFile(entry.name)) {
      nodes.push({
        name: entry.name,
        path: relativePath,
        type: "file"
      });
    }
  }
  return nodes.sort((a, b) => a.name.localeCompare(b.name));
}

export async function readTestFile(root: string, relativePath: string): Promise<string> {
  const safePath = sanitizePath(root, relativePath);
  return readFile(safePath, "utf8");
}

function sanitizePath(root: string, relativePath: string): string {
  const resolvedRoot = path.resolve(root);
  const resolved = path.resolve(resolvedRoot, relativePath);
  if (!resolved.startsWith(resolvedRoot)) {
    throw new Error("Invalid path");
  }
  return resolved;
}

async function safeReadDir(dir: string) {
  try {
    const entries = await readdir(dir, { withFileTypes: true });
    return entries;
  } catch (error) {
    console.warn(`[tests] Failed to read directory ${dir}`, error);
    return [];
  }
}

function isTestFile(filename: string): boolean {
  return (
    filename.endsWith(".rae") ||
    filename.endsWith(".txt") ||
    filename.endsWith(".out") ||
    filename.endsWith(".expect") ||
    filename.endsWith(".raepack")
  );
}
