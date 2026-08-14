import type { DownPublisher } from "./iot/client.js";

type Clip = {
  hz: number;
  chunks: Array<string | undefined>;
  bytes: number;
  timer: NodeJS.Timeout;
};

type SttOpts = {
  sttUrl: string;
  openaiApiKey: string;
};

function wavWrap(pcm: Buffer, hz: number): Buffer {
  const dataSize = pcm.length;
  const buf = Buffer.alloc(44 + dataSize);
  buf.write("RIFF", 0);
  buf.writeUInt32LE(36 + dataSize, 4);
  buf.write("WAVE", 8);
  buf.write("fmt ", 12);
  buf.writeUInt32LE(16, 16);
  buf.writeUInt16LE(1, 20);
  buf.writeUInt16LE(1, 22);
  buf.writeUInt32LE(hz, 24);
  buf.writeUInt32LE(hz * 2, 28);
  buf.writeUInt16LE(2, 32);
  buf.writeUInt16LE(16, 34);
  buf.write("data", 36);
  buf.writeUInt32LE(dataSize, 40);
  pcm.copy(buf, 44);
  return buf;
}

async function transcribeLocal(wav: Buffer, sttUrl: string): Promise<string> {
  const res = await fetch(`${sttUrl.replace(/\/$/, "")}/transcribe`, {
    method: "POST",
    headers: { "Content-Type": "audio/wav" },
    body: new Uint8Array(wav),
  });
  if (!res.ok) {
    throw new Error(`local stt ${res.status}`);
  }
  return (await res.text()).trim();
}

async function transcribeOpenAi(wav: Buffer, apiKey: string): Promise<string> {
  const form = new FormData();
  form.append("model", "whisper-1");
  form.append("language", "en");
  form.append(
    "file",
    new Blob([new Uint8Array(wav)], { type: "audio/wav" }),
    "hermes.wav",
  );
  const res = await fetch("https://api.openai.com/v1/audio/transcriptions", {
    method: "POST",
    headers: { Authorization: `Bearer ${apiKey}` },
    body: form,
  });
  if (!res.ok) {
    const err = await res.text();
    throw new Error(`whisper ${res.status}: ${err.slice(0, 200)}`);
  }
  const json = (await res.json()) as { text?: string };
  return (json.text || "").trim();
}

async function transcribe(wav: Buffer, opts: SttOpts): Promise<string> {
  try {
    const health = await fetch(`${opts.sttUrl.replace(/\/$/, "")}/health`, {
      signal: AbortSignal.timeout(800),
    });
    if (health.ok) {
      return await transcribeLocal(wav, opts.sttUrl);
    }
  } catch {
    /* local STT not running */
  }
  if (opts.openaiApiKey) return transcribeOpenAi(wav, opts.openaiApiKey);
  throw new Error("local STT not running (start scripts/setup-stt-ec2.sh)");
}

export function createVoiceInbox(opts: SttOpts, publishDown: DownPublisher) {
  const clips = new Map<string, Clip>();

  const finish = async (id: string) => {
    const clip = clips.get(id);
    if (!clip) return;
    clips.delete(id);
    clearTimeout(clip.timer);
    const parts: Buffer[] = [];
    for (let i = 0; i < clip.chunks.length; i++) {
      const d = clip.chunks[i];
      if (!d) {
        await publishDown({
          type: "transcript_error",
          message: "missing audio chunk",
        });
        return;
      }
      parts.push(Buffer.from(d, "base64"));
    }
    const pcm = Buffer.concat(parts);
    if (pcm.length < 320) {
      await publishDown({ type: "transcript_error", message: "too short" });
      return;
    }
    try {
      const wav = wavWrap(pcm, clip.hz || 16000);
      console.log(`[stt] clip id=${id} pcm=${pcm.length} hz=${clip.hz}`);
      const text = await transcribe(wav, opts);
      console.log(`[stt] text=${text.slice(0, 80)}`);
      await publishDown({ type: "transcript", text, append: true });
    } catch (err) {
      const msg = err instanceof Error ? err.message : String(err);
      console.warn("[stt] failed:", msg);
      await publishDown({ type: "transcript_error", message: msg.slice(0, 80) });
    }
  };

  return {
    begin(id: string, hz: number) {
      const prev = clips.get(id);
      if (prev) clearTimeout(prev.timer);
      clips.set(id, {
        hz: hz || 16000,
        chunks: [],
        bytes: 0,
        timer: setTimeout(() => void finish(id), 20_000),
      });
    },
    chunk(id: string, seq: number, d: string) {
      const clip = clips.get(id);
      if (!clip || typeof d !== "string") return;
      clip.chunks[seq] = d;
    },
    end(id: string) {
      if (!id) return;
      void finish(id);
    },
  };
}
