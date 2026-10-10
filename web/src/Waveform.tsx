import { useEffect, useState } from 'react';
import { X } from 'lucide-react';
import { headers } from './api';

interface Signal {
  name: string;
  points: { time: number; value: string }[];
}
// A bounded bus view for the example ALU. Full traces remain downloadable.
function parseVCD(text: string) {
  const signals = new Map<string, Signal>();
  let time = 0;
  let frames = 0;
  for (const line of text.split('\n')) {
    const variable = line.trim().match(/^\$var\s+\w+\s+\d+\s+(\S+)\s+(\w+)/);
    if (variable && ['a', 'b', 'op', 'result'].includes(variable[2])) {
      signals.set(variable[1], { name: variable[2], points: [] });
    }
    if (/^#\d+$/.test(line)) {
      if (++frames > 120) break;
      time = Number(line.slice(1));
    }
    const bus = line.match(/^b([01xz]+)\s+(\S+)/i);
    const scalar = line.match(/^([01xz])(\S+)/i);
    const change = bus || scalar;
    if (change) {
      const signal = signals.get(change[2]);
      if (!signal || signal.points.length >= 120) continue;
      const value = /^[01]+$/.test(change[1])
        ? `0x${parseInt(change[1], 2).toString(16).toUpperCase()}`
        : change[1];
      const last = signal.points.at(-1);
      if (last?.time === time) last.value = value;
      else if (last?.value !== value) signal.points.push({ time, value });
    }
  }
  return [...signals.values()].filter((s) => s.points.length).slice(0, 4);
}

export function Waveform({ job, name, close }: { job: string; name: string; close: () => void }) {
  const [signals, setSignals] = useState<Signal[]>([]);
  const [error, setError] = useState('');
  const [loading, setLoading] = useState(true);
  useEffect(() => {
    const controller = new AbortController();
    async function load() {
      try {
        const r = await fetch(`/api/jobs/${job}/artifacts/${encodeURIComponent(name)}`, {
          headers: headers(),
          signal: controller.signal,
        });
        if (!r.ok) throw new Error('Could not load this waveform.');
        if (Number(r.headers.get('Content-Length')) > 2_000_000)
          throw new Error(
            'This trace exceeds the inline viewer limit. Download it for a full viewer.',
          );
        const text = await r.text();
        if (text.length > 2_000_000) throw new Error('Download this trace for a full viewer.');
        const parsed = parseVCD(text);
        if (!parsed.length)
          throw new Error('No ALU bus signals found. Download for a full viewer.');
        setSignals(parsed);
      } catch (e) {
        if (!controller.signal.aborted) setError((e as Error).message);
      } finally {
        if (!controller.signal.aborted) setLoading(false);
      }
    }
    void load();
    return () => controller.abort();
  }, [job, name]);
  const max = Math.max(1, ...signals.flatMap((s) => s.points.map((p) => p.time)));
  const x = (time: number) => 85 + (time / max) * 580;
  return (
    <div className="waveform">
      <div className="waveform-heading">
        <strong>{name}</strong>
        <button onClick={close} aria-label="Close waveform">
          <X size={15} />
        </button>
      </div>
      {loading ? (
        <p>Loading waveform…</p>
      ) : error ? (
        <p role="alert">{error}</p>
      ) : (
        <>
          <div className="table-scroll">
            <svg
              viewBox={`0 0 680 ${signals.length * 48 + 30}`}
              role="img"
              aria-label={`${name}: measured ALU bus values over simulation time`}
            >
              {signals.map((s, i) => (
                <g key={s.name}>
                  <text x="10" y={i * 48 + 34}>
                    {s.name}
                  </text>
                  {s.points.map((p, n) => {
                    const width = Math.max(1, x(s.points[n + 1]?.time ?? max) - x(p.time));
                    return (
                      <g key={n}>
                        <rect
                          x={x(p.time)}
                          y={i * 48 + 16}
                          width={width}
                          height="25"
                          fill="#e8f1e1"
                          stroke="#679367"
                        />
                        <title>
                          {s.name} at {p.time}: {p.value}
                        </title>
                        {width > 30 && (
                          <text x={x(p.time) + 4} y={i * 48 + 33}>
                            {p.value}
                          </text>
                        )}
                      </g>
                    );
                  })}
                </g>
              ))}
              <text x="85" y={signals.length * 48 + 18}>
                0
              </text>
              <text x="665" y={signals.length * 48 + 18} textAnchor="end">
                {max} · VCD time units
              </text>
            </svg>
          </div>
          <p>
            First 120 simulation steps. Hover for vector values; download for the complete trace.
          </p>
        </>
      )}
    </div>
  );
}
