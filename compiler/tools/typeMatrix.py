#!/usr/bin/env python3
"""The type matrix: every element type x source x position as a function that checks its own value
(docs/generic-instances-design.md section 8). A prototype, not run by the suite yet: it writes one
program per element type into the given folder, `WRONG <name>` in an output is a wrong value, and
RAE_MEM_STATS=1 says what leaks.

    python3 compiler/tools/typeMatrix.py <folder>
"""
import os
import re
import sys

FOLDER = sys.argv[1] if len(sys.argv) > 1 else "/tmp/raeTypeMatrix"
TYPES={"Int":("3","4"),"String":('"a {1}"','"b {2}"'),"Bool":("true","false"),"Float":("1.5","2.5"),"UInt8":("7","9"),"Kind":("Kind.large","Kind.small"),
"Note":('noteOf(n: 1)','noteOf(n: 2)'),"List(Int)":("List(Int) { 1 }","List(Int) { 2 }"),"List(String)":('List(String) { "l {1}" }','List(String) { "m" }'),
"Pair(String)":('pairOf(value: "p {1}")','pairOf(value: "q")'),"StringMap(Int)":("mapOf(n: 1)","mapOf(n: 2)"),"Array(String, cap: 2)":("rowOf(n: 1)","rowOf(n: 2)"),"Any":('"any {1}"',"5")}
SOURCES={"local":("","one"),"own":("let moved: T = V1","own moved"),"call":("","make()"),"generic":("","same(value: one)"),"method":("","box.read()"),"field":("","box.held"),"element":("","list.copyAtFallback(index: 0, fallback: make())"),"map":("","map.values().copyAtFallback(index: 0, fallback: make())"),"match":("","match flag { case true => make(), default => one }"),"task":("let task: Task(T) = spawn make()","task.get()"),"literal":("","V1")}
POS={"let":"let x: T = SRC\n  check(at: AT, ok: x is V1)",
"var":"var x: T = V2\n  x = SRC\n  check(at: AT, ok: x is V1)",
"field":"let x: Box(T) = { held: SRC }\n  check(at: AT, ok: x.held is V1)",
"fieldSet":"var x: Box(T) = { held: V2 }\n  x.held = SRC\n  check(at: AT, ok: x.held is V1)",
"list":"let x: List(T) = [V2, SRC]\n  check(at: AT, ok: x.copyAtFallback(index: 1, fallback: V2) is V1)",
"add":"var x: List(T) = []\n  x.add(value: SRC)\n  check(at: AT, ok: x.copyAtFallback(index: 0, fallback: V2) is V1)",
"mapSet":"var x: StringMap(T) = {}\n  x.set(k: \"k\", value: SRC)\n  check(at: AT, ok: x.length is 1)",
"opt":"let x: opt T = SRC\n  check(at: AT, ok: x is V1)",
"viewArg":"check(at: AT, ok: byView(value: SRC))","ownArg":"check(at: AT, ok: byOwn(value: SRC))","copyArg":"check(at: AT, ok: byCopy(value: SRC))","genArg":"let x: T = same(value: SRC)\n  check(at: AT, ok: x is V1)",
"text":"check(at: AT, ok: \"{SRC}\".length() > 0)","is":"check(at: AT, ok: SRC is V1)","ret":"check(at: AT, ok: retAT() is V1)",
"defer":"var x: T = V2\n  defer {\n    x = SRC\n  }\n  check(at: AT, ok: true)","spawn":"let t: Task(Bool) = spawn byOwnTask(value: SRC)\n  check(at: AT, ok: t.get())"}
PROG_FILES={}
def prog(T):
    v1,v2=TYPES[T]
    out=['''open collections/StringMap
enum Kind { small, large }
type Note {
  text: String
  marks: List(Int)
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
''',
f"func make() ret {T} {{\n  ret {v1}\n}}\nfunc byView(value: view {T}) ret Bool {{\n  ret value is {v1}\n}}\nfunc byOwn(value: own {T}) ret Bool {{\n  let kept: {T} = own value\n  ret kept is {v1}\n}}\nfunc byOwnTask(value: own {T}) ret Bool {{\n  ret value is {v1}\n}}\nfunc byCopy(value: copy {T}) ret Bool {{\n  ret value is {v1}\n}}\n"]
    calls=[]
    for s,(pre,src) in SOURCES.items():
        for p,body in POS.items():
            if p=="defer" and s in("own","task"): continue
            if s=="own" and p in("viewArg","copyArg","genArg","text","is"): continue
            at=f"{s}{p[0].upper()+p[1:]}"
            b=body.replace("SRC",src).replace("AT()","AT()")
            setup=f"  let one: {T} = {v1}\n  let box: Box({T}) = {{ held: {v1} }}\n  let list: List({T}) = [{v1}]\n  var map: StringMap({T}) = {{}}\n  map.set(k: \"k\", value: {v1})\n"
            if pre: setup+="  "+pre.replace("V1",v1).replace("T)",T+")").replace(": T ",": "+T+" ")+"\n"
            if p=="ret":
                out.append(f"func ret{at}Inner(flag: view Bool) ret {T} {{\n{setup}  ret {src.replace('V1',v1)}\n}}\n")
                b=f'check(at: "{at}", ok: ret{at}Inner(flag: flag) is V1)'
                setup=""
            b=b.replace("AT",f'"{at}"').replace("V1",v1).replace("V2",v2).replace(": T =",f": {T} =").replace("(T)",f"({T})").replace("opt T",f"opt {T}")
            out.append(f"func {at}(flag: view Bool) {{\n{setup}  {b}\n}}\n"); calls.append(at)
    funcs=out[2:]; out=out[:2]
    files={}
    for f in funcs:
        name=re.match(r"func (?:ret)?([a-z]+)",f).group(1)
        files.setdefault(name,[]).append(f)
    PROG_FILES.clear(); PROG_FILES.update({k:"open collections/StringMap\n"+"".join(v) for k,v in files.items()})
    out.append("func main() {\n"+"".join(f"  {c}(flag: true)\n" for c in calls)+f'  log("done {len(calls)}")\n}}\n')
    return "".join(out),len(calls)
for T in TYPES:
    d=FOLDER+"/"+"".join(c for c in T if c.isalnum()); os.makedirs(d,exist_ok=True)
    s,n=prog(T); open(d+"/Main.rae","w").write(s)
    for k,v in PROG_FILES.items(): open(d+"/"+k.capitalize()+"Part.rae","w").write(v)
print(n, "per type,", len(TYPES), "types")
