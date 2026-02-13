export type ResourcePickerLabelLike = {
  dataset: {
    nodeId?: string;
    resourceType?: string;
    resourceIndex?: string;
    exposedResourceId?: string;
  };
};

export function findMatchingResourcePickerLabel<T extends ResourcePickerLabelLike>(
  candidates: ArrayLike<T> | null | undefined,
  nodeId: string,
  resourceType: "nam" | "ir",
  resourceIndex: number,
  exposedResourceId?: string,
): T | null {
  if (!candidates) {
    return null;
  }

  for (let labelIndex = 0; labelIndex < candidates.length; labelIndex += 1) {
    const candidate = candidates[labelIndex];
    if (!candidate) {
      continue;
    }

    if ((candidate.dataset.nodeId ?? "") !== nodeId) {
      continue;
    }

    const candidateType = candidate.dataset.resourceType as "nam" | "ir" | undefined;
    if (candidateType !== resourceType) {
      continue;
    }

    if (exposedResourceId && (candidate.dataset.exposedResourceId ?? "") !== exposedResourceId) {
      continue;
    }

    const candidateResourceIndexRaw = candidate.dataset.resourceIndex;
    const candidateResourceIndex = candidateResourceIndexRaw !== undefined
      ? parseInt(candidateResourceIndexRaw, 10)
      : 0;
    if (candidateResourceIndex !== resourceIndex) {
      continue;
    }

    return candidate;
  }

  return null;
}