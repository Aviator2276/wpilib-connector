// File-dialog filters arrive from Glass as space-separated glob lists, e.g.
// "*.jpg *.jpeg *.png *.json". Only "*" and "?" are used.

function toRegExp(glob: string): RegExp {
  const body = glob
    .replace(/[.+^${}()|[\]\\]/g, "\\$&")
    .replace(/\*/g, ".*")
    .replace(/\?/g, ".");
  return new RegExp(`^${body}$`, "i");
}

/** Returns a predicate for file names; no filters (or "*") matches everything. */
export function fileMatcher(filters: string[]): (name: string) => boolean {
  const globs = filters.flatMap((f) => f.split(/\s+/)).filter(Boolean);
  if (globs.length === 0 || globs.includes("*") || globs.includes("*.*")) {
    return () => true;
  }
  const res = globs.map(toRegExp);
  return (name) => res.some((re) => re.test(name));
}
