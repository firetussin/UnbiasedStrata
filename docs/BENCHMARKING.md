# Native community speed benchmark

`unbiased-bench` is a C++ HTTP client. It needs no Python interpreter, model weights, GPU toolkit or inference
engine on the client. Build it from the pinned submodule with a C++20 compiler and CMake:

```sh
git submodule update --init
make bench JOBS=2
make test-bench JOBS=2
build-frontend/bin/unbiased-bench --help
```

The build uses the repository's existing cpp-httplib and JSON dependencies. It fetches nothing at configure or
run time. `make test-bench` uses a native HTTP mock, not Python, a GPU or a live model. The client only sends HTTP
requests: it never starts/stops engines, changes their settings, downloads models or executes the advertised tools.
Start the server separately, using your normal sandbox and limits.

## Run

Use the origin URL, with or without a trailing `/v1`, and the model alias advertised by `/v1/models`:

```sh
# Set STRATA_KEY in your shell if the server requires a key. Do not put it in the command line or result files.
build-frontend/bin/unbiased-bench --url http://127.0.0.1:8080 --model my-model \
  --runs 3 --long --clients 2 \
  --engine-commit <commit-of-the-measured-engine> \
  --model-sha256 <sha256-of-the-GGUF> \
  --metadata machine-and-settings.json \
  --corpus-dir /path/to/public/repository \
  --notes 'No background compilation; power limits unchanged and not measured' \
  --out bench/results/my-new-run
```

`--out` must not exist. Every run has a new directory. Default output is `bench/results/native-<timestamp>/`.
Use `--api-key-env NAME` for a different key variable; its value is never written. HTTP is supported; this build
does not include a TLS client. Use a trusted local endpoint or tunnel for authenticated connections.
`--timeout` is a per-request deadline in seconds. `--think` leaves thinking enabled; the default sends both
`reasoning_effort: none` and `chat_template_kwargs.enable_thinking: false` on every request.

The engine commit is **not** inferred from the checkout used to build the benchmark client. The client records
its own build revision separately; supply the actual engine commit. Record modified builds and binary/model
hashes in the reproduction notes. A client built from a dirty tracked working tree is labeled `-dirty`.

Optional metadata uses a small allowlist of scalar fields, for example:

```json
{
  "backend": "HIP gfx1031",
  "engine_version": "0.1.45",
  "gpu": "AMD Radeon RX 6700 XT",
  "gpu_vram_mib": 12272,
  "cpu": "AMD Ryzen 5 3600",
  "installed_ram_gib": 32,
  "engine_threads": 8,
  "cpu_quota": "9 CPUs",
  "memory_limit_gib": 26,
  "kv": "backend default with --kv-whole",
  "vram_reserve_mib": 1024,
  "draft": "record the actual launch settings",
  "tail_skip": 6,
  "model_file": "Qwen3.6-35B-A3B-UD-Q3_K_M.gguf",
  "corpus_commit": "record the public corpus revision",
  "notes": "example values; replace with your measured setup"
}
```

Visible CPU, RAM, cgroup limits and the first visible DRM card are observations of the **client** machine.
They may differ from the inference host, especially across a network or container boundary. Identify the served
GPU with `--gpu-name` or metadata when detection is incomplete or several GPUs are visible. Use metadata for
actual installed RAM and inference limits. Unknown GPU detection is labeled unknown, never CPU-only.
No power consumption, peak VRAM, peak RAM or GPU-temperature claim is made by this tool.

## Workloads and protocol lineage

This is a native port of the speed-workload protocol from PolyStrata's `tools/strata_bench.py` schema1, plus its
`ab_engine.py`, `agent_turns_bench.py` and `serve_load.py` helpers, pinned to
`26e4cb5df7996edfb5ff549a63b82d8581e1e658` (MIT). Credit: VecSzn and PolyStrata contributors. Its result schema is
`unbiased-native-speed-1`, not the original Python tool's schema. Do not identify native output as an unmodified
official upstream report or silently mix the two protocols' tables.

| Stage | Workload |
| --- | --- |
| Excluded warm-up | One24-token chat,200/800-word token calibration, one8-token reply to a1K prompt |
| Short chat | The same three fixed questions;100-token cap; three repeats by default |
| Fresh prompts | Calibrated4,096/16,384-token targets;24-token output cap |
| Agent-style session | Same system prompt and four tool schemas; opening turn plus five document turns;64-token cap |
| Concurrency | Optional simultaneous clients,400-word essay request,128-token output cap; wall-clock burst throughput |
| Long prompt | Optional calibrated32,768-token target;24-token output cap |

The word-list generator reproduces CPython's `random.Random(7)` integer seed and choice sequence natively;
`std::mt19937(7)` would produce different words. Native tests check it against a frozen upstream sequence.
Calibration uses actual reported prompt tokens; target counts are approximate and actual counts are retained.
Every fresh prompt starts with a unique run tag. Contexts too small for a stage are explicitly marked skipped.
For a32K prompt test, configure a window larger than32K; the check includes an output/safety margin.

By default, agent files are deterministic generated fallback documents. With `--corpus-dir`, the same six
public-document names and750-word minimum/1,500-word maximum as the upstream client are considered, then missing
documents are filled deterministically. Only five files are used. Record the corpus revision; result data contains
file names, word counts and FNV-1a64 fingerprints (content identifiers, **not cryptographic checksums**).
Generated fallback bodies contain1,500 seeded words plus18 instruction words; the recorded count includes both.
Do not point this option at private material. Model answers remain in memory solely to continue each session;
they are not saved as chat transcripts. The tool schemas are never executed, so this is not an agent-quality test.

Transport and accounting differences from the reference are deliberate and versioned:

- A stream must end with `[DONE]`; truncated streams fail even when some text arrived.
- Missing generation/token/cache counters remain unknown. SSE chunks are never treated as tokens.
- Engine decode/prompt timings remain distinct from client TTFT and burst wall time. Decode falls back to a
  client estimate only when a real generation count and first-token time are available; the source is labeled.
- Calibration without valid prompt counts fails instead of silently estimating a workload.
- Native concurrency starts use a barrier. The server may queue requests; multiple clients do not prove parallel inference.
- The default corpus is generated rather than implicitly reading the benchmark checkout. Explicit corpora and
  the timestamp/tag implementation may change exact prompt tokens; use reported counts and corpus identifiers.

No hidden retries or output-based task selection are performed. Failures stay in the request journal and report.
The process returns nonzero for a failed/incomplete run; successful measurements alongside failures are still
available but must not be presented as a fully successful suite.

## Output, comparison and contribution

- `README.md`: human-readable results, scope, skips and failures.
- `results.json`: protocol/client/engine identities, metadata, calibration, all measured requests and summaries.
- `requests.jsonl`: one flushed record for **every** attempted request, including excluded warm-ups and calibration.

Medians and min/max ranges include only valid observations; every summary includes its observation count.
Empty/missing metrics stay null. A killed client can leave a partial JSONL journal; it is not a completed run.
Review denominators, skips, failures and unknown counters. Do not select only the fastest repeat.

```sh
build-frontend/bin/unbiased-bench --url http://127.0.0.1:8080 --model my-model \
  --out bench/results/another-run --compare bench/results/previous-run
```

`--compare` only accepts this native schema and prints the two summaries without claiming a speedup. Match model
file, quantization, context, corpus, thinking, cache, KV, speculation, reserve, threads, CPU/memory quotas and
background workloads before interpreting differences. No compile jobs or competing inference should run during
a measured comparison. Preserve failures instead of rerunning them out of the public record.

For a community-result PR, include those three files and concise reproduction notes under a new
`bench/results/<date>-community-<gpu>-<model>/` directory. Include the model checksum, engine/client revisions,
launch settings, corpus identity and resource limits. Keep speed-only results separate from local quality
benchmarks. The client scrubs its API key, local user/home/host and IPv4/bracketed IPv6 identifiers; it also omits
the server URL and arbitrary metadata keys. This cannot recognize every private string: manually review all
files, metadata and error bodies before publishing. Do not attach private logs, credentials or model weights.
