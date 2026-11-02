import {projectRegionDiagnostics} from './region_diagnostics.mjs';

const COVERAGE_SCHEMA = 'dynarmic.js/region-alias-coverage-v1';

function requireCondition(condition, message) {
  if (!condition) throw new Error(`region alias coverage: ${message}`);
}

// Compile-history evidence only: a "covered" entry is a candidate for a future
// bounded prototype, not an observed dynamic transfer or a performance win.
export function classifyRegionAliasCoverage(snapshot, {minimumCovered = 9} = {}) {
  requireCondition(snapshot?.schema === 'dynarmic.js/region-diagnostics-v2', 'requires region-diagnostics-v2');
  requireCondition(Number.isSafeInteger(minimumCovered) && minimumCovered >= 1,
    'minimumCovered must be a positive safe integer');
  projectRegionDiagnostics(snapshot);

  const selections = new Map();
  const laterEntries = [];
  for (const region of snapshot.regions) {
    const previous = selections.get(region.entryDescriptor) || [];
    if (previous.length > 0) {
      const staticallyEligible = region.frontend === 'a64' ? previous.filter(owner =>
        owner.region.frontend === 'a64' && owner.region.layout === 'fallback' &&
        owner.selectedIndex !== owner.region.entryIndex && owner.block.emitted && owner.block.returnSite) : [];
      const live = new Set(region.liveRegionIdsBeforeCompile);
      const liveEligibleOwnerRegionIds = staticallyEligible
        .filter(owner => live.has(owner.region.regionId))
        .map(owner => owner.region.regionId);
      laterEntries.push({
        regionId: region.regionId,
        entryDescriptor: region.entryDescriptor,
        previouslySelectedRegionIds: previous.map(owner => owner.region.regionId),
        staticallyEligibleRegionIds: staticallyEligible.map(owner => owner.region.regionId),
        liveEligibleOwnerRegionIds,
        covered: liveEligibleOwnerRegionIds.length > 0,
      });
    }
    for (let selectedIndex = 0; selectedIndex < region.blocks.length; ++selectedIndex) {
      const block = region.blocks[selectedIndex];
      const owners = selections.get(block.descriptor) || [];
      owners.push({region, selectedIndex, block});
      selections.set(block.descriptor, owners);
    }
  }

  const coveredLaterEntryCount = laterEntries.filter(row => row.covered).length;
  return {
    schema: COVERAGE_SCHEMA,
    totals: {
      regionCount: snapshot.regions.length,
      laterEntryCount: laterEntries.length,
      coveredLaterEntryCount,
    },
    decision: coveredLaterEntryCount >= minimumCovered ? 'GO' : 'STOP',
    laterEntries,
  };
}
