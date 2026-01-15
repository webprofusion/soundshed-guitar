export {};

declare global {
  interface JSZipObject {
    name: string;
    dir: boolean;
    async<T extends "arraybuffer" | "uint8array" | "blob">(type: T): Promise<
      T extends "arraybuffer" ? ArrayBuffer : T extends "uint8array" ? Uint8Array : Blob
    >;
  }

  interface JSZipLibrary {
    loadAsync(data: ArrayBuffer): Promise<{ files: Record<string, JSZipObject> }>;
  }

  interface Window {
    JSZip?: JSZipLibrary;
  }
}
