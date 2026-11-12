type Tone = "ok" | "wait" | "bad" | "off";

const TONES: Record<Tone, string> = {
  ok: "bg-ok",
  wait: "bg-wait animate-pulse",
  bad: "bg-bad",
  off: "bg-glass-muted/50",
};

/** A status light. The state is also given as text for screen readers. */
export function Dot({ tone, label }: { tone: Tone; label: string }) {
  return (
    <span className="inline-flex items-center">
      <span aria-hidden className={`inline-block size-2 rounded-full ${TONES[tone]}`} />
      <span className="sr-only">{label}</span>
    </span>
  );
}
