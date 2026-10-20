# UnbiasedStrata community speed benchmark

Native C++ port of the PolyStrata speed protocol; this is **not output from the original Python tool**. Speed only: no model-quality score or real tool execution.

- Measured: 2026-10-20T08:54:55Z
- Run complete: yes; failed requests/stages: 0
- Client revision: `b04952944883a7d4fa811117e2b02d63bc96a19f`
- Measured engine commit (supplied): `fe3b461183561150db3559c726292a33b64c1980`
- Model alias: `qwen3.6-35b-a3b-ud-q3_k_m`
- Context: 32768 tokens; thinking disabled; temperature0
- GPU: AMD Radeon RX 6700 XT; VRAM MiB: 12272
- CPU: AMD Ryzen 5 3600; visible RAM GiB: 31.256370544433594
- Background workloads / power / caveats: No background compilation or competing inference; power limits unchanged and not measured.

## Measurements

| Metric | Median (min–max; observations) |
| --- | ---: |
| chat decode tok/s | 74.84 (67.48–86.78; n=9) |
| chat TTFT s | 0.31 (0.23–0.33; n=9) |
| 4K prompt tok/s | 1334.41 (1327.96–1335.23; n=3) |
| 16K prompt tok/s | 1125.11 (1099.55–1125.55; n=3) |
| agent follow-up TTFT s | 3.51 (1.66–4.26; n=15) |
| agent decode tok/s | 67.78 (58.43–77.69; n=15) |
| concurrency total tok/s | 60.12 (59.29–60.87; n=3) |

## Method and limitations

Three fixed short questions, calibrated fresh 4K/16K prompts, optional 32K prompts, and a six-turn agent-style session; 3 repeats. The warm-up chat, two calibration requests and 1K warm-up prompt are excluded. No tool is executed. Loading is excluded. Engine timings and client TTFT are separate. Missing token counters remain unknown; SSE chunks are never counted as tokens. Failed/incomplete streams are retained and excluded from valid summaries.

Agent corpus mode: explicit repository documents with generated fallback. File names, word counts and non-cryptographic content fingerprints are in results.json. Different corpora, engine settings, contexts or quantizations are not controlled A/B comparisons. Concurrent clients may queue on this single-sequence engine; throughput is over burst wall time.

Skipped stages: `{"32K":"Context is 32768 tokens"}`

Failure details: `[]`

See results.json and requests.jsonl for every attempted request, timings, counts and supplied metadata. Review all files for private information before submitting. Reproduce with the documented options in [docs/BENCHMARKING.md](../../../docs/BENCHMARKING.md); exact run options are in results.json.
