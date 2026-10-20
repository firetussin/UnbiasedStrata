# UnbiasedStrata community speed benchmark

Native C++ port of the PolyStrata speed protocol; this is **not output from the original Python tool**. Speed only: no model-quality score or real tool execution.

- Measured: 2026-10-20T08:59:27Z
- Run complete: yes; failed requests/stages: 0
- Client revision: `b04952944883a7d4fa811117e2b02d63bc96a19f`
- Measured engine commit (supplied): `fe3b461183561150db3559c726292a33b64c1980`
- Model alias: `qwen3.6-35b-a3b-ud-q3_k_m`
- Context: 65536 tokens; thinking disabled; temperature0
- GPU: AMD Radeon RX 6700 XT; VRAM MiB: 12272
- CPU: AMD Ryzen 5 3600; visible RAM GiB: 31.256370544433594
- Background workloads / power / caveats: No background compilation or competing inference; power limits unchanged and not measured.

## Measurements

| Metric | Median (min–max; observations) |
| --- | ---: |
| chat decode tok/s | 78.26 (69.89–93.75; n=9) |
| chat TTFT s | 0.30 (0.26–0.35; n=9) |
| 4K prompt tok/s | 1316.39 (1302.80–1317.31; n=3) |
| 16K prompt tok/s | 1121.60 (1107.20–1126.46; n=3) |
| 32K prompt tok/s | 855.07 (846.42–855.42; n=3) |
| agent follow-up TTFT s | 3.50 (1.65–4.42; n=15) |
| agent decode tok/s | 69.19 (56.62–76.95; n=15) |
| concurrency total tok/s | 59.48 (57.31–59.71; n=3) |

## Method and limitations

Three fixed short questions, calibrated fresh 4K/16K prompts, optional 32K prompts, and a six-turn agent-style session; 3 repeats. The warm-up chat, two calibration requests and 1K warm-up prompt are excluded. No tool is executed. Loading is excluded. Engine timings and client TTFT are separate. Missing token counters remain unknown; SSE chunks are never counted as tokens. Failed/incomplete streams are retained and excluded from valid summaries.

Agent corpus mode: explicit repository documents with generated fallback. File names, word counts and non-cryptographic content fingerprints are in results.json. Different corpora, engine settings, contexts or quantizations are not controlled A/B comparisons. Concurrent clients may queue on this single-sequence engine; throughput is over burst wall time.

Skipped stages: `{}`

Failure details: `[]`

See results.json and requests.jsonl for every attempted request, timings, counts and supplied metadata. Review all files for private information before submitting. Reproduce with the documented options in [docs/BENCHMARKING.md](../../../docs/BENCHMARKING.md); exact run options are in results.json.
