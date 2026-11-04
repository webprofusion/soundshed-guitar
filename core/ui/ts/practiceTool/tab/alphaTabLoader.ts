/**
 * Loads alphaTab on first use.
 *
 * alphaTab is ~1.2 MB of script, and most sessions never open a tab, so it is
 * not part of the page: scripts/copy-vendor.js puts its browser build and the
 * Bravura music font in dist/vendor/alphatab/, and the first tab opened injects
 * the script. It is a classic script that defines the `alphaTab` global (the
 * UI has no bundler to resolve the package's ES module build); the types come
 * from the npm package, as a type-only import that is erased.
 */

import type * as AlphaTabModule from "@coderline/alphatab";

export type AlphaTabNamespace = typeof AlphaTabModule;

const VENDOR_DIR = "dist/vendor/alphatab/";

let loading: Promise<AlphaTabNamespace> | null = null;

/** An absolute URL for a file in the vendored alphaTab folder. alphaTab needs
 * absolute URLs: it builds its render worker from the script's address. */
export function alphaTabVendorUrl(file: string): string {
  return new URL(`${VENDOR_DIR}${file}`, document.baseURI).href;
}

function loadedNamespace(): AlphaTabNamespace | null {
  return (globalThis as { alphaTab?: AlphaTabNamespace }).alphaTab ?? null;
}

export function loadAlphaTab(): Promise<AlphaTabNamespace> {
  const existing = loadedNamespace();
  if (existing) {
    return Promise.resolve(existing);
  }
  loading ??= new Promise<AlphaTabNamespace>((resolve, reject) => {
    const script = document.createElement("script");
    script.src = alphaTabVendorUrl("alphaTab.min.js");
    script.async = true;
    script.onload = () => {
      const namespace = loadedNamespace();
      if (namespace) {
        resolve(namespace);
      } else {
        loading = null;
        reject(new Error("alphaTab loaded but did not define itself"));
      }
    };
    script.onerror = () => {
      loading = null;
      script.remove();
      reject(new Error("The tablature renderer could not be loaded"));
    };
    document.head.appendChild(script);
  });
  return loading;
}
