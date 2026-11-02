const DIAGNOSTIC_V1_SCHEMA = 'dynarmic.js/region-diagnostics-v1';
const DIAGNOSTIC_V2_SCHEMA = 'dynarmic.js/region-diagnostics-v2';
const DUPLICATION_SCHEMA = 'dynarmic.js/region-duplication-v1';
const U64_PATTERN = /^0x[0-9a-f]{16}$/;

function requireCondition(condition, message) {
  if (!condition) throw new Error(`region diagnostics: ${message}`);
}

function validateU64(value, field) {
  requireCondition(typeof value === 'string' && U64_PATTERN.test(value),
    `${field} must be a fixed-width lowercase 64-bit descriptor`);
}

function validateRegion(region, expectedId) {
  requireCondition(region && typeof region === 'object' && !Array.isArray(region), 'region must be an object');
  requireCondition(Number.isSafeInteger(region.regionId) && region.regionId === expectedId,
    `regionId sequence must be contiguous from 1 at ${expectedId}`);
  requireCondition(region.frontend === 'a32' || region.frontend === 'a64', 'frontend must be a32 or a64');
  validateU64(region.entryDescriptor, 'entryDescriptor');
  validateU64(region.entryPc, 'entryPc');
  requireCondition(Array.isArray(region.selectedDescriptors) && region.selectedDescriptors.length > 0,
    'selectedDescriptors must be a nonempty array');
  for (const descriptor of region.selectedDescriptors) validateU64(descriptor, 'selected descriptor');
  requireCondition(new Set(region.selectedDescriptors).size === region.selectedDescriptors.length,
    'duplicate descriptor within one region');
  requireCondition(Number.isSafeInteger(region.entryIndex) && region.entryIndex >= 0 &&
    region.entryIndex < region.selectedDescriptors.length, 'entryIndex outside selectedDescriptors');
  requireCondition(region.selectedDescriptors[region.entryIndex] === region.entryDescriptor,
    'entryDescriptor does not match entryIndex');
  requireCondition(Number.isSafeInteger(region.moduleBytes) && region.moduleBytes > 0,
    'moduleBytes must be a positive safe integer');
  requireCondition(Number.isSafeInteger(region.entryBodyBytes) && region.entryBodyBytes > 0 &&
    region.entryBodyBytes <= region.moduleBytes, 'entryBodyBytes must be positive and no larger than moduleBytes');
}

function validateV2Region(region) {
  requireCondition(region && typeof region === 'object' && !Array.isArray(region), 'v2 region must be an object');
  requireCondition(['single', 'structured', 'fallback'].includes(region.layout), 'invalid region layout');
  requireCondition(Array.isArray(region.blocks) && region.blocks.length > 0, 'v2 blocks must be nonempty');
  requireCondition((region.blocks.length === 1) === (region.layout === 'single'),
    'single layout must have exactly one block');
  for (const block of region.blocks) {
    requireCondition(block && typeof block === 'object' && !Array.isArray(block), 'v2 block must be an object');
    validateU64(block.descriptor, 'block descriptor');
    for (const field of ['emitted', 'entryReachable', 'returnSite']) {
      requireCondition(typeof block[field] === 'boolean', `block ${field} must be boolean`);
    }
    if (region.layout === 'structured') {
      requireCondition(block.emitted === block.entryReachable,
        'structured emitted blocks must equal entry-reachable blocks');
    }
    if (region.layout === 'fallback') requireCondition(block.emitted, 'fallback must emit every selected block');
  }
  const entry = region.blocks[region.entryIndex];
  requireCondition(entry && entry.emitted && entry.entryReachable, 'entry block must be emitted and reachable');
  requireCondition(Array.isArray(region.liveRegionIdsBeforeCompile), 'live region IDs must be an array');
  let previous = 0;
  for (const id of region.liveRegionIdsBeforeCompile) {
    requireCondition(Number.isSafeInteger(id) && id > previous && id < region.regionId,
      'live region IDs must be sorted, unique, safe earlier generations');
    previous = id;
  }
}

// Project the richer compile-boundary record without dropping or rewriting any
// v1 field. Validation is deliberately performed on both the v2-only metadata
// and this projection before any offline attribution uses it.
export function projectRegionDiagnostics(snapshot) {
  requireCondition(snapshot && (snapshot.schema === DIAGNOSTIC_V1_SCHEMA || snapshot.schema === DIAGNOSTIC_V2_SCHEMA),
    'schema mismatch');
  requireCondition(Array.isArray(snapshot.regions), 'regions must be an array');
  if (snapshot.schema === DIAGNOSTIC_V2_SCHEMA) snapshot.regions.forEach(validateV2Region);
  const projected = {
    schema: DIAGNOSTIC_V1_SCHEMA,
    regions: snapshot.regions.map(region => {
      const {layout, blocks, liveRegionIdsBeforeCompile, ...v1} = region;
      return snapshot.schema === DIAGNOSTIC_V2_SCHEMA
        ? {...v1, selectedDescriptors: blocks.map(block => block.descriptor)}
        : region;
    }),
  };
  projected.regions.forEach((region, index) => validateRegion(region, index + 1));
  return projected;
}

function descriptorEntry(index, descriptor) {
  if (!index.has(descriptor)) {
    index.set(descriptor, {descriptor, selectedRegionIds: [], entryRegionIds: [], duplicateSelectionCount: 0});
  }
  return index.get(descriptor);
}

function attributeRegion(region, index) {
  const duplicateDescriptors = [];
  for (const descriptor of region.selectedDescriptors) {
    const attributed = descriptorEntry(index, descriptor);
    if (attributed.selectedRegionIds.length > 0) {
      duplicateDescriptors.push({
        descriptor,
        firstSelectedRegionId: attributed.selectedRegionIds[0],
        previousSelectedRegionId: attributed.selectedRegionIds.at(-1),
      });
    }
    attributed.selectedRegionIds.push(region.regionId);
  }
  descriptorEntry(index, region.entryDescriptor).entryRegionIds.push(region.regionId);
  return {regionId: region.regionId, duplicateBlockCount: duplicateDescriptors.length, duplicateDescriptors};
}

function safeSum(values, field) {
  const total = values.reduce((sum, value) => sum + value, 0);
  requireCondition(Number.isSafeInteger(total), `${field} total exceeds safe integer range`);
  return total;
}

// Derive compilation-history duplication without mutating or approximating the
// raw records. Body bytes belong to whole regions and are never divided among
// guest blocks; repeated selections are not proof of byte-identical Wasm.
export function analyzeRegionDiagnostics(snapshot) {
  const projected = projectRegionDiagnostics(snapshot);
  const index = new Map();
  const regions = projected.regions.map(region => attributeRegion(region, index));
  const descriptors = [...index.values()].map(entry => ({
    ...entry,
    duplicateSelectionCount: entry.selectedRegionIds.length - 1,
  }));
  const selectedBlockCount = safeSum(projected.regions.map(region => region.selectedDescriptors.length),
    'selected block');
  return {
    schema: DUPLICATION_SCHEMA,
    totals: {
      regionCount: projected.regions.length,
      selectedBlockCount,
      uniqueDescriptorCount: descriptors.length,
      duplicatedDescriptorCount: descriptors.filter(entry => entry.duplicateSelectionCount > 0).length,
      duplicateSelectionCount: selectedBlockCount - descriptors.length,
      moduleBytes: safeSum(projected.regions.map(region => region.moduleBytes), 'module byte'),
      entryBodyBytes: safeSum(projected.regions.map(region => region.entryBodyBytes), 'entry body byte'),
    },
    descriptors,
    regions,
  };
}
