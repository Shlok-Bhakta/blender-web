#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Summarize Chromium render traces and WebGPU call/resource counters."""
import argparse
from collections import Counter
import json
from pathlib import Path


def cpu_summary(path):
    profiles = {}
    for event in json.loads(path.read_text())["traceEvents"]:
        if event["name"] != "ProfileChunk":
            continue
        key = (event["pid"], event["tid"], event.get("id"))
        nodes, samples = profiles.setdefault(key, ({}, Counter()))
        data = event["args"]["data"]
        cpu = data.get("cpuProfile", {})
        nodes.update((node["id"], node) for node in cpu.get("nodes", []))
        for sample, duration in zip(cpu.get("samples", []), data.get("timeDeltas", [])):
            samples[sample] += duration
    inclusive, own = Counter(), Counter()
    for nodes, samples in profiles.values():
        for sample, duration in samples.items():
            stack = []
            node = nodes.get(sample)
            while node:
                stack.append(node["callFrame"]["functionName"])
                node = nodes.get(node.get("parent"))
            # Exclude UI work and parked pthreads outside the native GPU job.
            if "blender_web_run_gpu_jobs" not in stack:
                continue
            own[stack[0]] += duration
            inclusive.update({name: duration for name in set(stack)})
    if not own:
        return {"nativeSymbolAttributionAvailable": False,
                "nativeSymbolAttributionNote": "No CPU samples had a named native GPU-job frame. C++ hotspot attribution is unavailable for this trace; enable BLENDER_WEB_DEBUG_NAMES for a named profiling build.",
                "hotspotsCpuMs": None, "sampledRenderCpuMs": None, "inclusiveCpuMs": {}}
    terms = {"shaderTranslation": "WebGPUShader::ensure_translated()",
             "shaderAdapter": "webgpu_adapt_draw_command(",
             "shadercCompilation": "CompileToSpecifiedOutputType(",
             "builtinSymbolSetup": "SetupBuiltinSymbolTable("}
    named_cpp = any("::" in name or name.startswith(("_ZN", "_ZL")) for name in inclusive)
    hotspots = ({label: round(sum(value for name, value in inclusive.items() if term in name)/1000, 2)
                 for label, term in terms.items()} if named_cpp else None)
    result = {"nativeSymbolAttributionAvailable": named_cpp, "hotspotsCpuMs": hotspots,
              "sampledRenderCpuMs": round(sum(own.values())/1000, 2),
              "inclusiveCpuMs": {name: round(value/1000, 2) for name, value in inclusive.most_common(35)}}
    if not named_cpp:
        result["nativeSymbolAttributionNote"] = "GPU-job CPU time is measurable, but C++ symbol names are absent. Named C++ hotspots require BLENDER_WEB_DEBUG_NAMES."
    return result


def summarize(directory):
    report = json.loads((directory / "acceptance.json").read_text())
    result = {key: report[key] for key in ("browserVersion", "bundleVersion", "renderSeconds", "settingsUnchanged", "browserErrors")}
    result["renderRuns"] = report.get("renderRuns", [report["renderSeconds"]])
    result["runs"] = []
    for run in range(len(result["renderRuns"])):
        workers = json.loads((directory / f"webgpu-profile-{run}.json").read_text())
        calls, waits = {}, {}
        for worker in workers:
            for source, target in (("calls", calls), ("waits", waits)):
                for name, values in (worker or {}).get(source, {}).items():
                    entry = target.setdefault(name, {"count": 0, "ms": 0})
                    entry["count"] += values["count"]
                    entry["ms"] += values["ms"]
        for entry in list(calls.values()) + list(waits.values()):
            entry["ms"] = round(entry["ms"], 3)
        stats = {"apiCpuCalls": calls, "asyncWaits": waits,
                 **cpu_summary(directory / f"browser-trace-{run}.json")}
        timestamps = directory / f'gpu-timestamps-{run}.json'
        if timestamps.exists():
            measurements = json.loads(timestamps.read_text())
            assert all(item['overflow'] == 0 for item in measurements), 'GPU timestamp capacity exceeded'
            stats['gpuTimestamps'] = measurements
            stats['gpuTimestampsNote'] = ('Pass times in milliseconds, with timestamp instrumentation overhead. '
                                          'Unsubmitted UI passes have zero timestamps and are excluded as invalid. '
                                          'These runs are not unprofiled performance baselines.')
        resources = directory / f"resources-{run}.json"
        if resources.exists():
            samples = json.loads(resources.read_text())
            busy = [s["gpuBusyPercent"] for s in samples if s["gpuBusyPercent"] is not None]
            if busy:
                stats["gpuBusyPercent"] = {"mean": round(sum(busy)/len(busy), 2), "max": max(busy), "samples": len(busy)}
        result["runs"].append(stats)
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    output = args.directory / "profile-summary.json"
    output.write_text(json.dumps(summarize(args.directory), indent=2)+"\n")
    print(output)
