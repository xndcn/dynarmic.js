#!/usr/bin/env node
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {collectResults,summarize} from './tune-arm.mjs';

const cell=value=>`"${String(value??'').replaceAll('"','""')}"`;
const fixed=value=>value===null||value===undefined?'incomplete':value.toFixed(3);
export function analyze(directory) {
  const spec=JSON.parse(fs.readFileSync(path.join(directory,'experiment.json'),'utf8'));
  assert.equal(spec.schema,'dynarmic.js/arm-tuning-v1');
  const observations=collectResults(directory,spec),summary=summarize(spec,observations);
  const lines=['# ARM QuickJS compiler comparison','',
    `${summary.completed}/${summary.planned} process observations completed; ${summary.failed} failed. Baseline: **${spec.baseline}**.`,
    `Each reported repeat is a fresh Node process. The upstream iteration count and the kernel harness's three samples are retained inside each observation; they are not counted as independent process repeats.`,
    '',`Environment: ${spec.environment.node} / ${spec.environment.platform} ${spec.environment.arch}; ${spec.environment.cpu}; ${spec.environment.cpuCount} logical CPUs; ${spec.environment.totalMemoryBytes} bytes RAM.`,
    '', '| Variant | Guest bytes | Size / baseline | Selected-metric speedup | Eight-suite speedup | Eight-suite round range |',
    '| --- | ---: | ---: | ---: | ---: | ---: |'];
  const baseline=summary.variants[spec.baseline];
  for(const [label,variant] of Object.entries(summary.variants))lines.push(`| ${label} | ${variant.guestBytes} | ${fixed(variant.guestBytes/baseline.guestBytes)}× | ${fixed(variant.geometricMeanPairedSpeedup)} | ${fixed(variant.eightSuiteWeightedSpeedup)} | ${fixed(variant.eightSuiteWeightedSpeedupSummary?.min)}–${fixed(variant.eightSuiteWeightedSpeedupSummary?.max)} |`);
  lines.push('','Speedup is baseline time / candidate time; above 1 favors the candidate. Within each process round the eight-suite value weights all eight official suites equally; the table reports the median and range across independent rounds. It is absent unless every case has the requested paired repeats. Selected-metric and ten-case aggregates use the same per-round-first calculation. Selected metrics also include each kernel separately when kernels/full was requested. All round ratios are retained in JSON.','',
    '| Variant | Case / metric | Paired processes | Candidate median | Speedup median | Speedup min–max |',
    '| --- | --- | ---: | ---: | ---: | ---: |');
  for(const [label,variant] of Object.entries(summary.variants))for(const [caseId,entry] of Object.entries(variant.cases))for(const [metric,value] of Object.entries(entry.metrics))
    lines.push(`| ${label} | ${caseId} / ${metric} | ${value.pairs.length} | ${fixed(value.median)} ms | ${fixed(value.pairedSpeedup?.median)} | ${fixed(value.pairedSpeedup?.min)}–${fixed(value.pairedSpeedup?.max)} |`);
  lines.push('','Inspect per-case regressions, the complete-call times, emitted Wasm sizes and compilation counters before selecting a candidate. One process round is a screen; it does not establish a stable performance improvement. Full observations, raw iteration counts, environment, executable hashes and original build manifests remain in the experiment directory.','');
  const columns=['round','position','variant','case','status','metric','valueMs','runMs','loadMs','compiledBlocks','emittedBytes','translateMs','optMs','emitMs','compileMs','instMs','attempt','error'];
  const rows=[columns.map(cell).join(',')];
  for(const entry of observations)for(const [metric,value] of Object.entries(entry.measurement?.metrics||{unavailable:null})) {
    const s=entry.measurement?.stats||{};
    rows.push([entry.task.round,entry.task.position,entry.task.variant,entry.task.caseId,entry.status,metric,value,entry.measurement?.runMs,entry.measurement?.loadMs,
      s.blocks,s.bytes,s.translateMs,s.optMs,s.emitMs,s.compileMs,s.instMs,entry.attempt,entry.error].map(cell).join(','));
  }
  return {summary,markdown:lines.join('\n'),csv:rows.join('\n')+'\n'};
}
if(process.argv[1]&&import.meta.url===pathToFileURL(process.argv[1]).href) {
  try {
    const [directory,option,prefix,...extra]=process.argv.slice(2);
    assert(directory&&option==='--output-prefix'&&prefix&&!extra.length,'Usage: node examples/quickjs/tune-arm-analyze.mjs EXPERIMENT_DIRECTORY --output-prefix NEW_PREFIX');
    const targets=['.json','.md','.csv'].map(extension=>path.resolve(prefix+extension));
    assert(targets.every(filename=>!fs.existsSync(filename)),'Analysis output exists; retain it and choose a new prefix');
    const result=analyze(path.resolve(directory));fs.mkdirSync(path.dirname(targets[0]),{recursive:true});
    [JSON.stringify(result.summary,null,2)+'\n',result.markdown,result.csv].forEach((text,index)=>fs.writeFileSync(targets[index],text,{flag:'wx'}));
    console.log(`Wrote ${targets.join(', ')}`);
  }catch(error){console.error(error.message);process.exitCode=2;}
}
