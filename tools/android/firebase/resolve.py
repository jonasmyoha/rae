#!/usr/bin/env python3
"""Resolve Firebase Messaging and what it depends on into artifacts.lock.

Run by hand when the pinned version changes (docs/push-notifications.md):

    python3 -I tools/android/firebase/resolve.py com.google.firebase:firebase-messaging:24.1.2

It reads the POMs from Google's Maven repository and Maven Central, follows the
compile and runtime dependencies (not optional, test or provided ones), keeps the
highest version of each artifact, and writes one line per artifact,
`group:artifact:version:extension sha256`, which fetch.sh downloads and checks.
This is what Gradle does for an app, without Gradle.
"""
import hashlib
import re
import sys
import urllib.request
import xml.etree.ElementTree as ElementTree
from pathlib import Path

repositories = ["https://dl.google.com/dl/android/maven2", "https://repo1.maven.org/maven2"]
namespace = "{http://maven.apache.org/POM/4.0.0}"


def fetch(path):
    for repository in repositories:
        try:
            with urllib.request.urlopen(f"{repository}/{path}", timeout=60) as response:
                return response.read()
        except Exception:
            continue
    return None


def artifactPath(group, artifact, version, extension):
    return f"{group.replace('.', '/')}/{artifact}/{version}/{artifact}-{version}.{extension}"


def versionKey(version):
    return [int(part) if part.isdigit() else -1 for part in re.split(r"[.-]", version)]


def plainVersion(version):
    # "[21.0.0]" is exactly that one; a range "[1.0,2.0)" takes its lower end
    version = version.strip()
    if version.startswith("[") or version.startswith("("):
        version = version[1:].rstrip("])").split(",")[0]
    return version.strip()


def text(element, name):
    found = element.find(namespace + name)
    return found.text.strip() if found is not None and found.text else None


poms = {}


def readPom(group, artifact, version):
    key = (group, artifact, version)
    if key not in poms:
        data = fetch(artifactPath(group, artifact, version, "pom"))
        if data is None:
            raise SystemExit(f"error: no POM for {group}:{artifact}:{version}")
        poms[key] = ElementTree.fromstring(data)
    return poms[key]


def properties(pom, group, artifact, version):
    values = {"project.version": version, "project.groupId": group, "project.artifactId": artifact}
    block = pom.find(namespace + "properties")
    if block is not None:
        for item in block:
            values[item.tag.replace(namespace, "")] = (item.text or "").strip()
    return values


def expand(value, values):
    if value is None:
        return None
    return re.sub(r"\$\{([^}]+)\}", lambda match: values.get(match.group(1), match.group(0)), value)


def dependencies(group, artifact, version):
    pom = readPom(group, artifact, version)
    values = properties(pom, group, artifact, version)
    found = []
    block = pom.find(namespace + "dependencies")
    if block is None:
        return found
    for dependency in block.findall(namespace + "dependency"):
        scope = text(dependency, "scope") or "compile"
        if scope not in ("compile", "runtime") or text(dependency, "optional") == "true":
            continue
        dependencyVersion = expand(text(dependency, "version"), values)
        if not dependencyVersion or "${" in dependencyVersion:
            raise SystemExit(f"error: {group}:{artifact}:{version} names {text(dependency, 'artifactId')} with no version")
        found.append((expand(text(dependency, "groupId"), values), expand(text(dependency, "artifactId"), values),
                      plainVersion(dependencyVersion)))
    return found


def extension(group, artifact, version):
    packaging = text(readPom(group, artifact, version), "packaging") or "jar"
    return "aar" if packaging == "aar" else "jar"


def main():
    root = sys.argv[1].split(":")
    chosen = {}
    waiting = [tuple(root)]
    # A newer version found later is resolved again, so its own dependencies count
    while waiting:
        group, artifact, version = waiting.pop(0)
        current = chosen.get((group, artifact))
        if current is not None and versionKey(current) >= versionKey(version):
            continue
        chosen[(group, artifact)] = version
        waiting.extend(dependencies(group, artifact, version))
    lines = []
    for (group, artifact), version in sorted(chosen.items()):
        kind = extension(group, artifact, version)
        data = fetch(artifactPath(group, artifact, version, kind))
        if data is None and kind == "jar":
            kind = "aar"
            data = fetch(artifactPath(group, artifact, version, kind))
        if data is None:
            raise SystemExit(f"error: no {kind} for {group}:{artifact}:{version}")
        lines.append(f"{group}:{artifact}:{version}:{kind} {hashlib.sha256(data).hexdigest()}")
    lock = Path(__file__).with_name("artifacts.lock")
    lock.write_text(f"# {sys.argv[1]}, resolved by resolve.py\n" + "\n".join(lines) + "\n")
    print(f"resolve: {len(lines)} artifacts into {lock}")


main()
