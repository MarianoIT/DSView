"""Compatibility for DSView decoders migrated to upstream wait() results."""
def match_mask(matches):
    """Convert the upstream condition tuple to the historical bit mask."""
    return sum(1 << index for index, matched in enumerate(matches or ()) if matched)
