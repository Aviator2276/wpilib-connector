// Every URL is derived from the document location, so the app works at a
// subdomain root and under Coder's path prefix alike. The daemon injects a
// <base> pointing at the app root, which document.baseURI reflects.

export function appBase(baseUri: string = document.baseURI): URL {
  return new URL(".", baseUri); // always ends with "/"
}

/** Absolute URL of a daemon endpoint, e.g. api("api/status"). */
export function api(path: string, baseUri?: string): string {
  return new URL(path.replace(/^\/+/, ""), appBase(baseUri)).toString();
}

/** Same, as a ws: / wss: URL. */
export function ws(path: string, baseUri?: string): string {
  const u = new URL(path.replace(/^\/+/, ""), appBase(baseUri));
  u.protocol = u.protocol === "https:" ? "wss:" : "ws:";
  return u.toString();
}

/** Router basepath: the app root's path without the trailing slash. */
export function routerBasepath(baseUri?: string): string {
  const p = appBase(baseUri).pathname.replace(/\/+$/, "");
  return p === "" ? "/" : p;
}
