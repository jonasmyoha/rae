#!/usr/bin/env python3
"""The type matrix: every element type x source x position as a function that checks its own value
(docs/generic-instances-design.md section 8). Two programs per type: `plain` writes the type out,
`generic` has the same functions with the type as `T`. Each is built once with AddressSanitizer and
UndefinedBehaviorSanitizer (plain where the C compiler has none) and run under RAE_MEM_STATS: a wrong
value, a leak, a sanitizer report, a crash or a C error fails the stage.

tools/type-matrix-known.txt lists what fails today, one `program function kind # why` a line (`*` and
`?` match); kind is `compile`, `crash` or `leak` (the function is left out of its program) or `value`.
A line is STALE, and fails the stage, when something it names passes: the list only gets shorter.

    tools/typeMatrix.py                    the stage (make -C compiler type-matrix)
    tools/typeMatrix.py --only generic/Int,plain/Any    some programs
    tools/typeMatrix.py --find ...         what fails, function by function, as lines for the list
    tools/typeMatrix.py --keep <folder>    the programs stay in that folder
"""
import argparse
import concurrent.futures
import fnmatch
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

COMPILER = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KNOWN_FILE = os.path.join(COMPILER, "tools", "type-matrix-known.txt")
RAE = os.environ.get("RAE_BIN") or os.path.join(COMPILER, "bin", "rae")
CC = os.environ.get("RAE_ASAN_CC", "clang")
KINDS = ("compile", "crash", "leak", "value")
SANITIZE = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all"]

# type: two different values
TYPES = {
    "Int": ("3", "4"),
    "String": ('"a {1}"', '"b {2}"'),
    "Bool": ("true", "false"),
    "Float": ("1.5", "2.5"),
    "UInt8": ("7", "9"),
    "Kind": ("Kind.large", "Kind.small"),
    "Note": ("noteOf(n: 1)", "noteOf(n: 2)"),
    "Label": ("labelOf(n: 1)", "labelOf(n: 2)"),
    "Point": ("pointOf(n: 1)", "pointOf(n: 2)"),
    "List(Int)": ("List(Int) { 1 }", "List(Int) { 2 }"),
    "List(String)": ('List(String) { "l {1}" }', 'List(String) { "m" }'),
    "Pair(String)": ('pairOf(value: "p {1}")', 'pairOf(value: "q")'),
    "StringMap(Int)": ("mapOf(n: 1)", "mapOf(n: 2)"),
    "Array(String, cap: 2)": ("rowOf(n: 1)", "rowOf(n: 2)"),
    "Any": ('"any {1}"', "5"),
}

# source: a statement before, and the expression (`results`: one of a function's several results). TYPE, V1, V2, MAKE, SPAWN are filled in.
SOURCES = {
    "local": ("", "one"),
    "own": ("let moved: TYPE = V1", "own moved"),
    "call": ("", "MAKE"),
    "generic": ("", "same(value: one)"),
    "method": ("", "box.read()"),
    "field": ("", "box.held"),
    "element": ("", "list.copyAtFallback(index: 0, fallback: MAKE)"),
    "map": ("", "map.values().copyAtFallback(index: 0, fallback: MAKE)"),
    "match": ("", "match flag { case true => MAKE, default => one }"),
    "task": ("let task: Task(TYPE) = spawn SPAWN", "task.get()"),
    "literal": ("", "V1"),
    "results": ("let taken: TYPE = value, let fine: Bool = fine = makeBoth()", "taken"),
}
# sources only the plain program has: a generic function has one result for now
PLAIN_ONLY = ("results",)

# position: statements with SRC; OK(...) is the check
POSITIONS = {
    "let": ["let x: TYPE = SRC", "OK(x is V1)"],
    "var": ["var x: TYPE = V2", "x = SRC", "OK(x is V1)"],
    "field": ["let x: Box(TYPE) = { held: SRC }", "OK(x.held is V1)"],
    "fieldSet": ["var x: Box(TYPE) = { held: V2 }", "x.held = SRC", "OK(x.held is V1)"],
    "list": ["let x: List(TYPE) = [V2, SRC]", "OK(x.copyAtFallback(index: 1, fallback: V2) is V1)"],
    "add": ["var x: List(TYPE) = []", "x.add(value: SRC)", "OK(x.copyAtFallback(index: 0, fallback: V2) is V1)"],
    "mapSet": ["var x: StringMap(TYPE) = {}", 'x.set(k: "k", value: SRC)', "OK(x.length is 1)"],
    "opt": ["let x: opt TYPE = SRC", "OK(x is V1)"],
    "viewArg": ["OK(byView(value: SRCOTHER))"],
    "ownArg": ["OK(byOwn(value: SRCOTHER))"],
    "copyArg": ["OK(byCopy(value: SRCOTHER))"],
    "genArg": ["let x: TYPE = same(value: SRC)", "OK(x is V1)"],
    "text": ['OK("{SRC}".length() > 0)'],
    "is": ["OK(SRC is V1)"],
    "ret": None,
    "defer": ["var x: TYPE = V2", "defer {", "  x = SRC", "}", "OK(true)"],
    "spawn": ["let t: Task(Bool) = spawn byOwnTask(value: SRCTASKOTHER)", "OK(t.get())"],
}

HEAD = """open collections/StringMap

enum Kind { small, large }

type Note {
  text: String
  marks: List(Int)
}

type Label {
  text: String
  kind: Kind
}

type Point {
  x: Int
  y: Float
}

type Pair(T: type) {
  first: T
}

type Box(T: type) {
  held: T
}

func noteOf(n: view Int) ret Note {
  ret Note { text: "note {n}", marks: { n } }
}

func pointOf(n: view Int) ret Point {
  ret Point { x: n, y: 2.5 }
}

func labelOf(n: view Int) ret Label {
  ret Label { text: "label {n}", kind: Kind.small }
}

func pairOf(T: type, value: view T) ret Pair(T) {
  ret Pair(T) { first: value }
}

func mapOf(n: view Int) ret StringMap(Int) {
  var map: StringMap(Int) = {}
  map.set(k: "k", value: n)
  ret map
}

func rowOf(n: view Int) ret Array(String, cap: 2) {
  var row: Array(String, cap: 2) = Array(String, cap: 2)
  row.set(index: 0, value: "row {n}")
  ret row
}

func same(T: type, value: view T) ret T {
  ret value
}

func read(T: type, this: view Box(T)) ret T {
  ret this.held
}

func check(at: view String, ok: view Bool) {
  if ok is false {
    log("WRONG {at}")
  }
}
"""

PLAIN_HELPERS = """
func make() ret TYPE {
  ret V1
}

func makeBoth() ret value: TYPE, fine: Bool {
  ret value: V1, fine: true
}

func byView(value: view TYPE) ret Bool {
  ret value is V1
}

func byOwn(value: own TYPE) ret Bool {
  let kept: TYPE = own value
  ret kept is V1
}

func byOwnTask(value: own TYPE) ret Bool {
  ret value is V1
}

func byCopy(value: copy TYPE) ret Bool {
  ret value is V1
}
"""

GENERIC_HELPERS = """
func copied(T: type, value: view T) ret T {
  let held: T = value
  ret held
}

func handBack(T: type, value: own T) ret T {
  ret value
}

func byView(T: type, value: view T, other: view T) ret Bool {
  ret value is other
}

func byOwn(T: type, value: own T, other: view T) ret Bool {
  let kept: T = own value
  ret kept is other
}

func byOwnTask(T: type, value: own T, other: own T) ret Bool {
  ret value is other
}

func byCopy(T: type, value: copy T, other: view T) ret Bool {
  ret value is other
}
"""


class Program:
    """One type, plain or generic: its functions by name, as Rae text."""

    def __init__(self, form, type_name):
        self.type_name = type_name
        self.generic = form == "generic"
        self.name = form + "/" + "".join(c for c in type_name if c.isalnum())
        self.value1, self.value2 = TYPES[type_name]
        self.functions = {}
        for source, (before, expression) in SOURCES.items():
            for position, lines in POSITIONS.items():
                if self.generic and source in PLAIN_ONLY:
                    continue
                # what Rae refuses: a move in a defer, `own` where nothing takes it
                if position == "defer" and source in ("own", "task"):
                    continue
                if source == "own" and position in ("viewArg", "copyArg", "genArg", "text", "is"):
                    continue
                name = source + position[0].upper() + position[1:]
                self.functions[name] = self.function(name, before, expression, lines)

    def fill(self, text):
        if self.generic:
            words = {"MAKE": "copied(value: first)", "SPAWN": "handBack(value: copied(value: first))",
                     "SRCTASKOTHER": "SRC, other: copied(value: first)", "SRCOTHER": "SRC, other: first",
                     "TYPE": "T", "V1": "first", "V2": "second"}
        else:
            words = {"MAKE": "make()", "SPAWN": "make()", "SRCTASKOTHER": "SRC", "SRCOTHER": "SRC",
                     "TYPE": self.type_name, "V1": self.value1, "V2": self.value2}
        for word, replacement in words.items():
            text = text.replace(word, replacement)
        return text

    def function(self, name, before, expression, lines):
        setup = ["let one: TYPE = V1", "let box: Box(TYPE) = { held: V1 }", "let list: List(TYPE) = [V1]",
                 "var map: StringMap(TYPE) = {}", 'map.set(k: "k", value: V1)']
        if before:
            setup.append(before)
        parameters = "T: type, first: view T, second: view T, flag: view Bool" if self.generic else "flag: view Bool"
        text = ""
        if lines is None:
            arguments = "first: first, second: second, flag: flag" if self.generic else "flag: flag"
            text += f"func {name}Inner({parameters}) ret TYPE {{\n"
            text += "".join("  " + line + "\n" for line in setup) + f"  ret {expression}\n}}\n\n"
            setup, lines = [], [f"OK({name}Inner({arguments}) is V1)"]
        text += f"func {name}({parameters}) {{\n"
        for line in setup + lines:
            checked = re.fullmatch(r"OK\((.*)\)", line)
            text += "  " + (f'check(at: "{name}", ok: {checked.group(1)})' if checked else line) + "\n"
        text = self.fill(text + "}\n")
        return self.fill(text.replace("SRC", expression))

    def write(self, folder, names):
        """The program of those functions in `folder`, a file per source: {file: [(last line, function)]}."""
        os.makedirs(folder, exist_ok=True)
        arguments = "first: first, second: second, flag: true" if self.generic else "flag: true"
        main = HEAD + (GENERIC_HELPERS if self.generic else self.fill(PLAIN_HELPERS)) + "\nfunc main() {\n"
        if self.generic:
            main += f"  let first: {self.type_name} = {self.value1}\n  let second: {self.type_name} = {self.value2}\n"
        main += "".join(f"  {name}({arguments})\n" for name in names) + f'  log("done {len(names)}")\n}}\n'
        parts, owner = {"Main": main}, {}
        for name in names:
            part = re.match("[a-z]+", name).group().capitalize() + "Part"
            parts[part] = parts.get(part, "open collections/StringMap\n") + "\n" + self.functions[name]
            owner.setdefault(part, []).append((parts[part].count("\n"), name))
        for part, text in parts.items():
            with open(os.path.join(folder, part + ".rae"), "w") as file:
                file.write(text)
        return owner


class Runner:
    def __init__(self, folder):
        self.folder = folder
        self.seconds = {"emit": 0.0, "cc": 0.0, "run": 0.0}
        self.environment = dict(os.environ, RAE_FORMAT="off", RAE_PROGRESS="off", RAE_TOOLCHAIN_CHECK="off",
                                RAE_MEM_STATS="1", RAE_CHECK_CLONES="1", ASAN_OPTIONS="detect_leaks=0:use_sigaltstack=0")
        self.flags = ["-std=c11", "-O0", "-w", "-ffp-contract=off", "-ferror-limit=0", "-I/opt/homebrew/include"]
        self.libraries = ["-lm", "-lpthread"]
        if sys.platform == "darwin":
            self.libraries += ["-framework", "Foundation", "-framework", "ImageIO", "-framework", "CoreGraphics"]
        probe = os.path.join(folder, "probe.c")
        with open(probe, "w") as file:
            file.write("int main(void) { return 0; }\n")
        self.sanitized = self.command([CC, *SANITIZE, probe, "-o", probe + ".out"], 60)[0] == 0
        if self.sanitized:
            self.flags += SANITIZE + ["-gline-tables-only", "-fno-omit-frame-pointer"]
        self.runtime = None

    def command(self, arguments, timeout, stage=None, cwd=None):
        start = time.time()
        try:
            done = subprocess.run(arguments, env=getattr(self, "environment", None), cwd=cwd, timeout=timeout,
                                  stdin=subprocess.DEVNULL, capture_output=True, text=True, errors="replace")
            code, output = done.returncode, done.stdout + done.stderr
        except subprocess.TimeoutExpired:
            code, output = 124, f"no end after {timeout} s"
        if stage:
            self.seconds[stage] += time.time() - start
        return code, output

    def build_runtime(self):
        """The runtime as one object file, for every program."""
        program = Program("plain", "Int")
        folder = os.path.join(self.folder, "runtime")
        program.write(os.path.join(folder, "source"), ["localLet"])
        code, output = self.emit(folder)
        if code:
            sys.exit("type-matrix: the first program does not compile\n" + output)
        self.runtime = os.path.join(folder, "runtime.o")
        code, output = self.command([CC, *self.flags, "-c", os.path.join(folder, "c", "rae_runtime.c"),
                                     "-I" + os.path.join(folder, "c"), "-o", self.runtime], 300, "cc")
        if code:
            sys.exit("type-matrix: the runtime does not compile\n" + output)

    def emit(self, folder):
        source, out = os.path.join(folder, "source"), os.path.join(folder, "c")
        os.makedirs(out, exist_ok=True)
        return self.command([RAE, "build", "--target", "compiled", "--emit-c", "--project", source, "--out",
                             os.path.join(out, "out.c"), os.path.join(source, "Main.rae")], 120, "emit")

    def run(self, program, names, folder):
        """(kind, output, functions named by the output): kind None when all is right."""
        shutil.rmtree(folder, ignore_errors=True)
        owner = program.write(os.path.join(folder, "source"), names)
        code, output = self.emit(folder)
        if code:
            named = set()
            for part, line in re.findall(r"(\w+Part)\.rae:(\d+):", output):
                named |= {name for last, name in owner.get(part, []) if int(line) <= last
                          and not any(int(line) <= other < last for other, _ in owner[part])}
            return "compile", output, named
        c_file, app = os.path.join(folder, "c", "out.c"), os.path.join(folder, "app")
        code, output = self.command([CC, *self.flags, c_file, self.runtime, "-I" + os.path.join(folder, "c"),
                                     *self.libraries, "-o", app], 600, "cc")
        if code:
            return "compile", output, self.functions_of(c_file, output, names)
        code, output = self.command([app], 120, "run", cwd=os.path.join(folder, "source"))
        wrong = set(re.findall(r"^WRONG (\w+)$", output, re.M))
        if code or "Sanitizer" in output or "runtime error:" in output:
            return "crash", output, set()
        if f"done {len(names)}" not in output:
            return "crash", "it did not come to its end\n" + output, set()
        leaks = sum(int(n) for n in re.findall(r"(?:TOTAL|mem:buf).*?outstanding=(-?\d+)", output))
        if leaks:
            return "leak", output, wrong
        return ("value" if wrong else None), output, wrong

    def functions_of(self, c_file, output, names):
        """The matrix functions in whose C the compiler reported an error."""
        with open(c_file) as file:
            lines = file.read().split("\n")
        starts, current = [], None
        known = set(names)
        for line in lines:
            if line.startswith("RAE_UNUSED static") and line.endswith("{"):
                name = re.search(r"rae_[A-Z][a-z]*Part_([A-Za-z]+?)(?:Inner)?(?:_|\()", line)
                current = name.group(1) if name and name.group(1) in known else None
            starts.append(current)
        numbers = [int(n) for n in re.findall(r"out\.c:(\d+):\d+: error", output)]
        return {starts[n - 1] for n in numbers if n <= len(starts) and starts[n - 1]}


def read_known():
    known = []
    if not os.path.exists(KNOWN_FILE):
        return known
    with open(KNOWN_FILE) as file:
        for number, line in enumerate(file, 1):
            words = line.split("#")[0].split()
            if not words:
                continue
            if len(words) != 3 or words[2] not in KINDS:
                sys.exit(f"type-matrix-known.txt:{number}: not `program function kind`: {line.strip()}")
            known.append((number, *words))
    return known


def known_lines(known, program, name):
    return [entry for entry in known
            if fnmatch.fnmatchcase(program.name, entry[1]) and fnmatch.fnmatchcase(name, entry[2])]


def short(output, count=6):
    kept = [line for line in output.split("\n") if re.search(r"error|WRONG|Sanitizer|no end|not come", line)]
    return "\n".join("    " + line[:240] for line in (kept or output.split("\n"))[:count])


def find_failures(runner, program, names, folder):
    """[(name, kind, output)], each of `names` on its own."""
    found = []
    for name in names:
        kind, output, _ = runner.run(program, [name], os.path.join(folder, "one"))
        if kind:
            found.append((name, kind, output))
    return found


def check_program(runner, program, known):
    """Problems as text: what fails beside the list, and what the list names but passes."""
    folder = os.path.join(runner.folder, program.name)
    problems, apart, values = [], {}, {}
    for name in program.functions:
        entries = known_lines(known, program, name)
        for entry in entries[:1]:
            (values if entry[3] == "value" else apart)[name] = entry
    names = [name for name in program.functions if name not in apart]
    if names:
        kind, output, named = runner.run(program, names, os.path.join(folder, "all"))
        new = sorted(named - set(values))
        if kind in ("compile", "crash", "leak"):
            problems.append(f"{program.name}: {kind}" + (f" in {', '.join(new[:8])}" if new else "") + "\n" + short(output))
        elif new:
            problems.append(f"{program.name}: wrong value in {', '.join(new[:8])}")
        if kind in (None, "value"):
            for name in sorted(set(values) - named):
                problems.append(f"STALE: {program.name} {name} has the right value: shorten line {values[name][0]}")
    # what the list names: those without an error of their own in one program are tried alone
    left = [name for name in apart if apart[name][3] == "compile"]
    while left:
        kind, _, named = runner.run(program, left, os.path.join(folder, "known"))
        if kind != "compile" or not named & set(left):
            break
        left = [name for name in left if name not in named]
    left += [name for name in apart if apart[name][3] != "compile"]
    failing = {name for name, _, _ in find_failures(runner, program, left, folder)}
    for name in left:
        if name not in failing:
            problems.append(f"STALE: {program.name} {name} passes: shorten line {apart[name][0]}")
    return problems


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--only", help="programs, with commas: plain/Int,generic/*")
    parser.add_argument("--find", action="store_true")
    parser.add_argument("--keep", help="a folder the programs stay in")
    options = parser.parse_args()
    programs = [Program(form, name) for form in ("plain", "generic") for name in TYPES]
    if options.only:
        wanted = options.only.split(",")
        programs = [p for p in programs if any(fnmatch.fnmatchcase(p.name, w) for w in wanted)]
    known = read_known()
    folder = os.path.abspath(options.keep) if options.keep else tempfile.mkdtemp(prefix="rae_type_matrix_")
    os.makedirs(folder, exist_ok=True)
    start, problems = time.time(), []
    try:
        runner = Runner(folder)
        runner.build_runtime()
        jobs = int(os.environ.get("RAE_TEST_JOBS") or os.cpu_count() or 4)
        with concurrent.futures.ThreadPoolExecutor(min(jobs, len(programs) or 1)) as pool:
            if options.find:
                def all_of(program):
                    return find_failures(runner, program, list(program.functions), os.path.join(folder, program.name))
                for program, found in zip(programs, pool.map(all_of, programs)):
                    for name, kind, output in found:
                        why = (re.findall(r"error: .*|:\d+: .*|WRONG.*|SUMMARY.*", output) or [""])[0][:100]
                        print(f"{program.name} {name} {kind} # {why}", flush=True)
                return
            for found in pool.map(lambda program: check_program(runner, program, known), programs):
                problems += found
    finally:
        if not options.keep:
            shutil.rmtree(folder, ignore_errors=True)
    if not options.only:
        for number, program_name, name, _ in known:
            if not any(known_lines([(number, program_name, name, "")], p, f) for p in programs for f in p.functions):
                problems.append(f"STALE: line {number} of type-matrix-known.txt names nothing")
    for problem in problems:
        print(problem)
    functions = sum(len(p.functions) for p in programs)
    apart = sum(1 for p in programs for f in p.functions if known_lines(known, p, f))
    spent = ", ".join(f"{stage} {seconds:.0f} s" for stage, seconds in runner.seconds.items())
    summary = (f"type-matrix ({len(programs)} programs, {functions} combinations, {apart} known failures, "
               f"{'AddressSanitizer' if runner.sanitized else 'no AddressSanitizer here'}, "
               f"{time.time() - start:.0f} s; {spent} over all jobs)")
    if problems:
        print(f"FAIL: {summary}: {len(problems)} problems")
        sys.exit(1)
    print(f"PASS: {summary}")


main()
