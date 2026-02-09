export {};

declare global {
  interface JSZipObject {
    name: string;
    dir: boolean;
    async<T extends "arraybuffer" | "uint8array" | "blob" | "text">(type: T): Promise<
      T extends "arraybuffer"
        ? ArrayBuffer
        : T extends "uint8array"
          ? Uint8Array
          : T extends "blob"
            ? Blob
            : string
    >;
  }

  interface JSZipInstance {
    files: Record<string, JSZipObject>;
    file(name: string): JSZipObject | null;
    file(name: string, data: string | ArrayBuffer | Uint8Array, options?: { base64?: boolean }): JSZipInstance;
    folder(name: string): JSZipInstance | null;
    generateAsync(options: { type: "blob" }): Promise<Blob>;
  }

  interface JSZipLibrary {
    new(): JSZipInstance;
    loadAsync(data: ArrayBuffer): Promise<JSZipInstance>;
  }

  interface Window {
    JSZip?: JSZipLibrary;
  }
}
