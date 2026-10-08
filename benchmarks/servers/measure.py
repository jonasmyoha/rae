"""The measuring half of run.sh (docs/server-benchmarks-design.md §8).

run.sh checks the dependencies and the load average and builds the servers;
this starts them, checks them, drives the load tools, samples RSS and CPU
time, and writes build/raw/*.json, results/summary.json and
results/metadata.json. Run it through run.sh, which sets its environment.
"""
import json
import os
import platform
import re
import signal
import socket
import statistics
import subprocess
import sys
import threading
import time

HERE = os.environ["HERE"]
BUILD = os.environ["BUILD"]
CACHE = os.environ["CACHE"]
RAE_BIN = os.environ["RAE_BIN"]
RAE_ROOT = os.environ["RAE_ROOT"]
OHA = os.path.join(CACHE, "bin", "oha")
WRK = os.path.join(CACHE, "bin", "wrk")
RESULTS = os.environ.get("BENCH_RESULTS", os.path.join(HERE, "results"))
RAW = os.path.join(BUILD, "raw")

SECONDS = int(os.environ.get("BENCH_SECONDS", "15"))
RUNS = int(os.environ.get("BENCH_RUNS", "5"))
WARMUP = int(os.environ.get("BENCH_WARMUP", "5"))
CONNECTIONS = int(os.environ.get("BENCH_CONNECTIONS", "256"))
LOADGEN_THREADS = int(os.environ.get("BENCH_LOADGEN_THREADS", "4"))
PIPELINE_DEPTH = 16
PERFORMANCE_CORES = int(os.environ["PERFORMANCE_CORES"])
IMPLS = os.environ.get("BENCH_IMPLS", "rae-eventLoop rae-ecs rust node bun").split()
CASES = os.environ.get("BENCH_CASES", "plaintext json pipelined").split()
WORKERS = [int(w) for w in os.environ.get("BENCH_WORKERS", "1 %d" % PERFORMANCE_CORES).split()]

SOURCES = {
    "rae-eventLoop": "rae/eventLoop/http/Main.rae",
    "rae-ecs": "rae/ecs/http/Main.rae",
    "rust": "rust/src/bin/http.rs",
    "node": "javascript/node/Http.js",
    "bun": "javascript/bun/Http.js",
}


def command_for(impl):
    if impl.startswith("rae-"):
        return [os.path.join(BUILD, impl, "server")]
    if impl == "rust":
        return [os.path.join(BUILD, "rust", "release", "http")]
    if impl == "node":
        return ["node", os.path.join(HERE, "javascript", "node", "Http.js")]
    if impl == "bun":
        return ["bun", os.path.join(HERE, "javascript", "bun", "Http.js")]
    raise ValueError(impl)


def free_port():
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    return port


def output_of(command):
    try:
        return subprocess.run(command, capture_output=True, text=True, timeout=30).stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        return ""


# ----- servers -----------------------------------------------------------------


class Server:
    def __init__(self, impl, workers):
        self.impl = impl
        self.workers = workers
        self.port = free_port()
        self.log_path = os.path.join(RAW, "%s-w%d.log" % (impl, workers))
        environment = dict(os.environ, PORT=str(self.port), WORKERS=str(workers), RAE_BENCH_STATS="1")
        self.log = open(self.log_path, "w")
        self.process = subprocess.Popen(
            command_for(impl), env=environment, stdout=self.log, stderr=subprocess.STDOUT, start_new_session=True
        )

    def wait_ready(self):
        for _ in range(200):
            if self.process.poll() is not None:
                return False
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=0.2).close()
                return True
            except OSError:
                time.sleep(0.05)
        return False

    def check(self):
        result = subprocess.run(
            ["sh", os.path.join(HERE, "check.sh"), str(self.port), "%s/w%d" % (self.impl, self.workers)],
            capture_output=True,
            text=True,
            timeout=120,
        )
        print(result.stdout.rstrip())
        return result.returncode == 0

    def stop(self):
        try:
            os.killpg(self.process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(self.process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            self.process.wait(timeout=5)
        self.log.close()

    def stats(self):
        """Per worker (requests, allocations) from the last RAE_BENCH_STATS lines"""
        latest = {}
        with open(self.log_path) as log:
            for line in log:
                match = re.match(r"stats worker=(\d+) requests=(\d+) allocations=(\d+)", line)
                if match:
                    latest[int(match.group(1))] = (int(match.group(2)), int(match.group(3)))
        return latest


def cpu_seconds(text):
    # ps `time`: [[dd-]hh:]mm:ss.ss
    days = 0
    if "-" in text:
        day_text, text = text.split("-", 1)
        days = int(day_text)
    total = 0.0
    for part in text.split(":"):
        total = total * 60 + float(part)
    return days * 86400 + total


def group_usage(process_group):
    """(RSS in KB, CPU seconds) summed over a process group"""
    output = output_of(["ps", "-A", "-o", "pgid=,rss=,time="])
    rss = 0
    cpu = 0.0
    for line in output.splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[0] == str(process_group):
            rss += int(fields[1])
            cpu += cpu_seconds(fields[2])
    return rss, cpu


class Sampler(threading.Thread):
    """Peak RSS of the server's process group, sampled every 100 ms"""

    def __init__(self, process_group):
        super().__init__(daemon=True)
        self.process_group = process_group
        self.peak_kb = 0
        self.running = True

    def run(self):
        while self.running:
            rss, _ = group_usage(self.process_group)
            self.peak_kb = max(self.peak_kb, rss)
            time.sleep(0.1)


# ----- load ----------------------------------------------------------------------


def run_oha(port, path, seconds):
    output = subprocess.run(
        [
            OHA, "--no-tui", "--output-format", "json", "-z", "%ds" % seconds, "-c", str(CONNECTIONS),
            "--worker-threads", str(LOADGEN_THREADS), "http://127.0.0.1:%d%s" % (port, path),
        ],
        capture_output=True,
        text=True,
        timeout=seconds + 60,
    ).stdout
    data = json.loads(output)
    statuses = data.get("statusCodeDistribution", {})
    good = statuses.get("200", 0)
    percentiles = data["latencyPercentiles"]
    return {
        "requestsPerSecond": good / data["summary"]["total"],
        "p50Ms": (percentiles.get("p50") or 0) * 1000,
        "p99Ms": (percentiles.get("p99") or 0) * 1000,
        "p999Ms": (percentiles.get("p99.9") or 0) * 1000,
        "responses": good,
        "errors": sum(statuses.values()) - good,
    }


def run_wrk(port, seconds):
    output = subprocess.run(
        [
            WRK, "-t", str(LOADGEN_THREADS), "-c", str(CONNECTIONS), "-d", "%ds" % seconds,
            "-s", os.path.join(HERE, "pipeline.lua"), "http://127.0.0.1:%d/plaintext" % port,
            "--", str(PIPELINE_DEPTH),
        ],
        capture_output=True,
        text=True,
        timeout=seconds + 60,
    ).stdout
    line = [text for text in output.splitlines() if text.startswith("{")][-1]
    data = json.loads(line)
    # Throughput only: wrk 4.2's latency histogram is wrong under pipelining
    # (its upper percentiles read 0), so no latency is reported for this case
    return {
        "requestsPerSecond": data["responses"] / (data["durationUs"] / 1e6),
        "p50Ms": None,
        "p99Ms": None,
        "p999Ms": None,
        "responses": data["responses"],
        "errors": data["errors"],
    }


def load(case, port, seconds):
    if case == "plaintext":
        return run_oha(port, "/plaintext", seconds)
    if case == "json":
        return run_oha(port, "/json", seconds)
    return run_wrk(port, seconds)


def measure(server, case):
    """One timed run: the load tool's numbers plus RSS, CPU and allocations"""
    process_group = server.process.pid
    stats_before = server.stats()
    _, cpu_before = group_usage(process_group)
    sampler = Sampler(process_group)
    sampler.start()
    result = load(case, server.port, SECONDS)
    sampler.running = False
    sampler.join()
    _, cpu_after = group_usage(process_group)
    result["peakRssMb"] = sampler.peak_kb / 1024
    result["cpuSeconds"] = cpu_after - cpu_before
    if server.impl.startswith("rae-"):
        # The stats lines come once a second; wait for the one after the run
        time.sleep(1.2)
        stats_after = server.stats()
        requests = sum(stats_after[w][0] - stats_before.get(w, (0, 0))[0] for w in stats_after)
        allocations = max(a for _, a in stats_after.values()) - max(
            [a for _, a in stats_before.values()] or [0]
        )
        result["allocationsPerRequest"] = allocations / requests if requests else None
    return result


# ----- summary -------------------------------------------------------------------


def summarise(runs):
    numbers = {}
    for key in ["requestsPerSecond", "p50Ms", "p99Ms", "p999Ms", "peakRssMb", "cpuSeconds", "allocationsPerRequest"]:
        values = [run[key] for run in runs if run.get(key) is not None]
        if values:
            numbers[key] = {"median": statistics.median(values), "min": min(values), "max": max(values)}
    numbers["errors"] = sum(run["errors"] for run in runs)
    return numbers


def ratio(ecs, event_loop, key):
    if key not in ecs or key not in event_loop or not event_loop[key]["median"]:
        return None
    return ecs[key]["median"] / event_loop[key]["median"]


def source_lines(path):
    code = 0
    total = 0
    with open(os.path.join(HERE, path)) as source:
        for line in source:
            total += 1
            stripped = line.strip()
            if stripped and not stripped.startswith(("#", "//")):
                code += 1
    return {"lines": total, "codeLines": code}


def metadata(load_before):
    def version(command):
        return output_of(command).splitlines()[0] if output_of(command) else None

    return {
        "date": time.strftime("%Y-%m-%d"),
        "machine": {
            "model": output_of(["sysctl", "-n", "hw.model"]),
            "cpu": output_of(["sysctl", "-n", "machdep.cpu.brand_string"]),
            "cores": os.cpu_count(),
            "performanceCores": PERFORMANCE_CORES,
            "memoryGb": round(int(output_of(["sysctl", "-n", "hw.memsize"]) or 0) / 2**30),
        },
        "os": "%s %s" % (output_of(["sw_vers", "-productName"]), output_of(["sw_vers", "-productVersion"]))
        or platform.platform(),
        "loadAverage": {"before": float(load_before), "after": os.getloadavg()[0]},
        "rae": {
            "version": version([RAE_BIN, "--version"]),
            "commit": output_of(["git", "-C", RAE_ROOT, "rev-parse", "HEAD"]),
            "dirty": bool(output_of(["git", "-C", RAE_ROOT, "status", "--porcelain", "--", "lib", "compiler", "benchmarks/servers"])),
        },
        "toolchains": {
            "cc": version(["cc", "--version"]),
            "rustc": version(["rustc", "--version"]),
            "node": version(["node", "--version"]),
            "bun": version(["bun", "--version"]),
            "oha": version([OHA, "--version"]),
            "wrk": version([WRK, "-v"]) if os.path.exists(WRK) else None,
        },
        "settings": {
            "seconds": SECONDS,
            "runs": RUNS,
            "warmupSeconds": WARMUP,
            "connections": CONNECTIONS,
            "pipelineDepth": PIPELINE_DEPTH,
            "loadGeneratorThreads": LOADGEN_THREADS,
            "workers": WORKERS,
        },
        "limits": "load generator on the same machine (competes for cores); macOS, no pinning; "
        "Bun's reusePort does not balance connections across processes on macOS",
    }


def main():
    os.makedirs(RAW, exist_ok=True)
    os.makedirs(RESULTS, exist_ok=True)
    cases = [case for case in CASES if case != "pipelined" or os.path.exists(WRK)]
    if "pipelined" in CASES and "pipelined" not in cases:
        print("skipping the pipelined case: wrk is not built (sh fetch.sh)")
    summary = {"date": time.strftime("%Y-%m-%d"), "results": {}, "ecsOverEventLoop": {}, "sourceLines": {}}
    for impl in IMPLS:
        summary["sourceLines"][impl] = source_lines(SOURCES[impl])
    for workers in WORKERS:
        servers = {}
        try:
            for impl in IMPLS:
                server = Server(impl, workers)
                if not server.wait_ready():
                    print("%s/w%d did not start; see %s" % (impl, workers, server.log_path))
                    server.stop()
                    continue
                if not server.check():
                    print("%s/w%d failed check.sh: not timed" % (impl, workers))
                    server.stop()
                    continue
                servers[impl] = server
            for case in cases:
                print("== %d worker(s), %s" % (workers, case))
                for impl, server in servers.items():
                    load(case, server.port, WARMUP)
                runs = {impl: [] for impl in servers}
                for run in range(RUNS):
                    order = list(servers)
                    if run % 2 == 1 and "rae-eventLoop" in order and "rae-ecs" in order:
                        # Alternate the Rae styles so drift favours neither
                        first, second = order.index("rae-eventLoop"), order.index("rae-ecs")
                        order[first], order[second] = order[second], order[first]
                    for impl in order:
                        result = measure(servers[impl], case)
                        runs[impl].append(result)
                        latency = ""
                        if result["p50Ms"] is not None:
                            latency = "  p50 %6.2f ms  p99 %6.2f ms" % (result["p50Ms"], result["p99Ms"])
                        print(
                            "  %-14s run %d: %10.0f req/s%s  rss %6.1f MB%s"
                            % (
                                impl, run + 1, result["requestsPerSecond"], latency, result["peakRssMb"],
                                "  %.2f alloc/req" % result["allocationsPerRequest"]
                                if result.get("allocationsPerRequest") is not None
                                else "",
                            )
                        )
                key = "w%d-%s" % (workers, case)
                summary["results"][key] = {}
                for impl, impl_runs in runs.items():
                    with open(os.path.join(RAW, "%s-%s.json" % (impl, key)), "w") as raw:
                        json.dump(impl_runs, raw, indent=2)
                    summary["results"][key][impl] = summarise(impl_runs)
                if "rae-ecs" in runs and "rae-eventLoop" in runs:
                    ecs = summary["results"][key]["rae-ecs"]
                    event_loop = summary["results"][key]["rae-eventLoop"]
                    overlap = not (
                        ecs["requestsPerSecond"]["max"] < event_loop["requestsPerSecond"]["min"]
                        or event_loop["requestsPerSecond"]["max"] < ecs["requestsPerSecond"]["min"]
                    )
                    summary["ecsOverEventLoop"][key] = {
                        "requestsPerSecond": ratio(ecs, event_loop, "requestsPerSecond"),
                        "p50Ms": ratio(ecs, event_loop, "p50Ms"),
                        "p99Ms": ratio(ecs, event_loop, "p99Ms"),
                        "peakRssMb": ratio(ecs, event_loop, "peakRssMb"),
                        "verdict": "no measurable difference" if overlap else "measurable difference",
                    }
        finally:
            for server in servers.values():
                server.stop()
    with open(os.path.join(RESULTS, "summary.json"), "w") as out:
        json.dump(summary, out, indent=2)
        out.write("\n")
    with open(os.path.join(RESULTS, "metadata.json"), "w") as out:
        json.dump(metadata(os.environ.get("LOAD", "0")), out, indent=2)
        out.write("\n")
    print("wrote %s/summary.json and metadata.json" % RESULTS)


if __name__ == "__main__":
    sys.exit(main())
