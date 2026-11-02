import assert from 'node:assert/strict';
import {projectRegionDiagnostics} from '../../../tests/bench/region_diagnostics.mjs';

const A64_SINGLE_STEP_BIT = 1n << 57n;

// Replay the candidate's no-overwrite publication contract. This experiment's
// reports have no invalidation: every earlier generation is live at every miss.
// Histories with owner loss need a richer record and are rejected, not guessed.
export function replayAliasHistory(snapshot) {
  projectRegionDiagnostics(snapshot);
  const ownerByDescriptor = new Map();
  const regions = [];
  for (const region of snapshot.regions) {
    const expectedLive = Array.from({length: region.regionId - 1}, (_, index) => index + 1);
    assert.deepEqual(region.liveRegionIdsBeforeCompile, expectedLive,
      `region ${region.regionId}: invalidation or missing live earlier owner is outside this static gate`);
    assert(!ownerByDescriptor.has(region.entryDescriptor),
      `region ${region.regionId}: original entry ${region.entryDescriptor} was already published`);

    ownerByDescriptor.set(region.entryDescriptor, region.regionId);
    const aliases = [];
    const conflictingAliases = [];
    if (region.frontend === 'a64' && region.layout === 'fallback' &&
        (BigInt(region.entryDescriptor) & A64_SINGLE_STEP_BIT) === 0n) {
      for (const block of region.blocks) {
        if (!block.emitted || !block.returnSite || block.descriptor === region.entryDescriptor) continue;
        const liveOwner = ownerByDescriptor.get(block.descriptor);
        if (liveOwner !== undefined) {
          conflictingAliases.push({descriptor: block.descriptor, ownerRegionId: liveOwner});
        } else {
          ownerByDescriptor.set(block.descriptor, region.regionId);
          aliases.push(block.descriptor);
        }
      }
    }
    regions.push({
      regionId: region.regionId,
      entryDescriptor: region.entryDescriptor,
      aliases,
      conflictingAliases,
    });
  }
  return {ownerByDescriptor, regions};
}
