# Cache-pruning mechanism follow-up plan

Registered: 2026-10-27 after the original mechanism gate failed and before any
baseline/candidate performance timing. This is a separate mechanism experiment;
it does not retroactively pass or erase the original gate.

## Original failed observation

The [original protocol](2026-10-27-cache-pruning-performance-plan.md) required
identical A32 and A64 region histories from one run per implementation. A64 met
that requirement: both reports contained 111 regions with identical histories.
The initial A32 reports contained 117 baseline and 118 candidate regions, so the
original gate **failed and timing was not started**.

Two diagnostic repeats, not performance samples, show that equal region counts
were not a valid A32 baseline assumption:

| Implementation | Initial regions | Repeat regions |
| --- | ---: | ---: |
| Baseline | 117 | 118 |
| Candidate | 118 | 116 |

The A32 union of full entry descriptors is identical across implementations.
All entries absent from an individual run are fallback regions. One fallback
entry appears in the baseline repeat but not its initial run; the candidate also
varies across its own two runs. Earlier project evidence had already recorded
A32 compiled-path variability. No failed report is discarded.

## Replacement mechanism gate

Use all four retained A32 diagnostic reports and both A64 reports. Before any
timing, require:

1. The original normal-halt and native/guest CRC checks remain valid.
2. A64 retains the original strict requirement: identical ordered region
   histories and masks, no per-region growth, unchanged fallback/single sizes,
   and at least one shrinking structured region with omitted blocks.
3. For A32, the union of full entry descriptors across the two baseline runs
   must exactly equal the union across the two candidate runs. Every entry not
   common to all four runs must have fallback layout wherever observed.
4. For every baseline/candidate pairing and every entry shared by that pair,
   layout and all selected/emitted/reachable/return-site masks must match;
   module bytes and entry-body bytes must not grow. Fallback and single-block
   sizes must remain unchanged. At least one common structured region with an
   omitted block must shrink.
5. Preserve a comparison of all four cross-implementation pairings and all raw
   reports. Do not compare whole-run A32 byte totals as a static mechanism gate,
   because their compiled region populations differ. The subsequent paired
   production experiment still applies its unchanged emitted-byte budget to
   actual matched cases.

If this replacement gate fails, stop before timing. If it passes, proceed with
the already registered smoke and complete-duration timing protocol unchanged.
The kernel, Run, compile and emitted-byte performance budgets are not modified.
