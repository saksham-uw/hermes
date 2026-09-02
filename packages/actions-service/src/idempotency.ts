import { mkdir, readFile, rename, writeFile } from "node:fs/promises";
import { dirname } from "node:path";

type StoredRecord = {
  fingerprint: string;
  value: unknown;
  updatedAt: string;
};

type StoredData = {
  version: 1;
  records: Record<string, StoredRecord>;
};

export class IdempotencyConflictError extends Error {}

export class IdempotencyStore {
  private data: StoredData = { version: 1, records: {} };
  private loaded = false;
  private writeChain: Promise<void> = Promise.resolve();

  constructor(private readonly file: string | null) {}

  private async load(): Promise<void> {
    if (this.loaded) return;
    this.loaded = true;
    if (!this.file) return;
    try {
      const parsed = JSON.parse(await readFile(this.file, "utf8")) as StoredData;
      if (parsed.version === 1 && parsed.records) this.data = parsed;
    } catch (error) {
      if ((error as NodeJS.ErrnoException).code !== "ENOENT") throw error;
    }
  }

  async get<T>(key: string, fingerprint: string): Promise<T | undefined> {
    await this.load();
    const record = this.data.records[key];
    if (!record) return undefined;
    if (record.fingerprint !== fingerprint) {
      throw new IdempotencyConflictError(
        "Idempotency key was already used for a different operation",
      );
    }
    return structuredClone(record.value) as T;
  }

  async put<T>(key: string, fingerprint: string, value: T): Promise<void> {
    await this.load();
    const current = this.data.records[key];
    if (current && current.fingerprint !== fingerprint) {
      throw new IdempotencyConflictError(
        "Idempotency key was already used for a different operation",
      );
    }
    this.data.records[key] = {
      fingerprint,
      value: structuredClone(value),
      updatedAt: new Date().toISOString(),
    };
    if (!this.file) return;
    this.writeChain = this.writeChain.then(async () => {
      const folder = dirname(this.file!);
      await mkdir(folder, { recursive: true });
      const temporary = `${this.file!}.${process.pid}.tmp`;
      await writeFile(temporary, `${JSON.stringify(this.data, null, 2)}\n`, {
        mode: 0o600,
      });
      await rename(temporary, this.file!);
    });
    await this.writeChain;
  }
}
