import React, { useEffect, useState } from 'react';
import { createRoot } from 'react-dom/client';
import {
  ArrowUpRight,
  Check,
  ChevronRight,
  Download,
  GitBranch,
  Grid2X2,
  Layers,
  LoaderCircle,
  Plus,
  RefreshCw,
  Server,
  Square,
  Terminal,
  X,
} from 'lucide-react';
import { api, download, headers } from './api';
import type { Build, GridState, Job, Status } from './types';
import './style.css';
import { Waveform } from './Waveform';

const empty: GridState = { builds: [], workers: [], releases: [], regression_threshold: 10 };
const done = (s: Status) => ['passed', 'failed', 'cancelled'].includes(s);
const repoName = (repo: string) =>
  repo
    .replace(/\.git$/, '')
    .split('/')
    .filter(Boolean)
    .slice(-2)
    .join('/');
const duration = (n: number) => (n > 60 ? `${(n / 60).toFixed(1)}m` : `${n.toFixed(2)}s`);
const bytes = (n: number) =>
  n > 1048576 ? `${(n / 1048576).toFixed(1)} MB` : `${(n / 1024).toFixed(1)} KB`;
const when = (s: string) =>
  new Date(s).toLocaleString(undefined, {
    month: 'short',
    day: 'numeric',
    hour: '2-digit',
    minute: '2-digit',
  });
function Badge({ status }: { status: string }) {
  return (
    <span className={`badge ${status}`}>
      <span className="status-dot" />
      {status.replaceAll('_', ' ')}
    </span>
  );
}

function App() {
  const [data, setData] = useState(empty),
    [selected, setSelected] = useState(''),
    [page, setPage] = useState('builds'),
    [filter, setFilter] = useState('all'),
    [search, setSearch] = useState(''),
    [form, setForm] = useState(false),
    [error, setError] = useState(''),
    [connectionError, setConnectionError] = useState(''),
    [connected, setConnected] = useState(false),
    [busy, setBusy] = useState(false),
    [token, setToken] = useState(sessionStorage.getItem('regressgrid.token') || ''),
    [settings, setSettings] = useState(false);
  async function refresh() {
    try {
      const d = await api<GridState>('/api/state');
      setData(d);
      setConnected(true);
      setConnectionError('');
      setSelected((s) => s || d.builds[0]?.id || '');
    } catch (e) {
      setConnected(false);
      setConnectionError((e as Error).message);
    }
  }
  useEffect(() => {
    void refresh();
    const timer = setInterval(() => void refresh(), 2000);
    return () => clearInterval(timer);
  }, []);
  const build = data.builds.find((b) => b.id === selected),
    jobs = data.builds.flatMap((b) => b.jobs),
    active = data.workers.filter((w) => w.healthy),
    counts = (s: string) => jobs.filter((j) => j.status === s).length;
  async function action(path: string, body?: unknown) {
    setBusy(true);
    setError('');
    try {
      const res = await api<Build>(path, 'POST', body);
      if (res.id?.startsWith('b_')) setSelected(res.id);
      await refresh();
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  }
  const filtered = data.builds.filter(
    (b) =>
      (filter === 'all' || b.status === filter) &&
      (b.repository + ' ' + b.commit + ' ' + b.id).toLowerCase().includes(search.toLowerCase()),
  );
  const clusters = new Map<string, Job[]>();
  for (const j of jobs) {
    if (j.failure_signature && j.status === 'failed')
      clusters.set(j.failure_signature, [...(clusters.get(j.failure_signature) || []), j]);
  }
  return (
    <>
      <header className="app-header">
        <a
          className="brand"
          href="#"
          onClick={(e) => {
            e.preventDefault();
            setPage('builds');
          }}
        >
          <Grid2X2 size={24} />
          <strong>RegressGrid</strong>
          <span className="brand-tag">Build operations</span>
        </a>
        <div className="header-right">
          <span className={`connection ${connected ? 'online' : ''}`}>
            <span />
            {connected ? 'Control plane connected' : 'Control plane unavailable'}
          </span>
          <button className="connection-button" onClick={() => setSettings((v) => !v)}>
            Connection
          </button>
        </div>
      </header>
      <div className="shell">
        <nav className="main-nav" aria-label="Main navigation">
          {[
            { id: 'builds', icon: Layers, label: 'Builds' },
            { id: 'workers', icon: Server, label: 'Workers' },
            { id: 'releases', icon: ArrowUpRight, label: 'Releases' },
          ].map((n) => (
            <button
              key={n.id}
              className={page === n.id ? 'active' : ''}
              onClick={() => setPage(n.id)}
            >
              <n.icon size={17} />
              {n.label}
              <span>
                {n.id === 'builds'
                  ? data.builds.length
                  : n.id === 'workers'
                    ? active.length
                    : data.releases.length}
              </span>
            </button>
          ))}
          <span className="nav-note">C++ build & regression platform</span>
        </nav>
        {settings && (
          <form
            className="connection-form"
            onSubmit={(e) => {
              e.preventDefault();
              if (token) sessionStorage.setItem('regressgrid.token', token);
              else sessionStorage.removeItem('regressgrid.token');
              void refresh();
              setSettings(false);
            }}
          >
            <label>
              API token
              <input
                type="password"
                value={token}
                onChange={(e) => setToken(e.target.value)}
                placeholder="Leave blank for local development"
                autoComplete="off"
              />
            </label>
            <button className="primary" type="submit">
              Connect
            </button>
            <p>Stored in this browser session. Workers use their own token.</p>
          </form>
        )}
        {(error || connectionError) && (
          <div role="alert" className="error-banner">
            <span>{error || connectionError}</span>
            {connectionError && (
              <button onClick={() => setSettings(true)}>Update connection</button>
            )}
            <button aria-label="Dismiss error" onClick={() => setError('')}>
              <X size={16} />
            </button>
          </div>
        )}
        <main>
          <div className="page-heading">
            <div>
              <h1>
                {page === 'builds'
                  ? 'Build overview'
                  : page === 'workers'
                    ? 'Worker fleet'
                    : 'Release history'}
              </h1>
              <p>
                {page === 'builds'
                  ? 'Follow every configuration from commit to verified artifact.'
                  : page === 'workers'
                    ? 'Capacity, capabilities, and heartbeat health across your fleet.'
                    : 'Versioned artifacts promoted from a passing build matrix.'}
              </p>
            </div>
            <div className="heading-actions">
              <button className="quiet" aria-label="Refresh data" onClick={() => void refresh()}>
                <RefreshCw size={16} />
              </button>
              <button
                className="primary"
                onClick={() => {
                  setForm((v) => !v);
                  setPage('builds');
                }}
              >
                <Plus size={17} />
                {form ? 'Close submission' : 'Submit build'}
              </button>
            </div>
          </div>
          {form && (
            <SubmitForm
              data={data}
              onClose={() => setForm(false)}
              onSubmit={async (body) => {
                setBusy(true);
                try {
                  const b = await api<Build>('/api/builds', 'POST', body);
                  setSelected(b.id);
                  setForm(false);
                  await refresh();
                } finally {
                  setBusy(false);
                }
              }}
              busy={busy}
            />
          )}
          {page === 'builds' && (
            <>
              <div className="status-strip">
                {(['queued', 'running', 'passed', 'failed'] as Status[]).map((s) => (
                  <button
                    key={s}
                    className={filter === s ? 'chosen' : ''}
                    onClick={() => setFilter(filter === s ? 'all' : s)}
                  >
                    <span className={`strip-dot ${s}`} />
                    <span>{s[0].toUpperCase() + s.slice(1)} jobs</span>
                    <strong>{counts(s)}</strong>
                  </button>
                ))}
                <div className="fleet-summary">
                  <Server size={17} />
                  <span>
                    <strong>{active.length}</strong> workers online
                  </span>
                  <small>
                    {active.reduce((n, w) => n + w.active_jobs, 0)} /{' '}
                    {active.reduce((n, w) => n + w.slots, 0)} slots in use
                  </small>
                </div>
              </div>
              <div className="build-layout">
                <section className="panel build-list">
                  <div className="panel-heading">
                    <h2>
                      Recent builds <span>{data.builds.length}</span>
                    </h2>
                    <select
                      aria-label="Filter builds"
                      value={filter}
                      onChange={(e) => setFilter(e.target.value)}
                    >
                      <option value="all">All statuses</option>
                      {['queued', 'running', 'passed', 'failed', 'cancelled'].map((s) => (
                        <option key={s} value={s}>
                          {s}
                        </option>
                      ))}
                    </select>
                  </div>
                  <div className="search">
                    <input
                      aria-label="Search builds"
                      placeholder="Search repository, commit, or build…"
                      value={search}
                      onChange={(e) => setSearch(e.target.value)}
                    />
                  </div>
                  <div className="builds">
                    {filtered.map((b) => (
                      <button
                        className={`build-row ${b.id === selected ? 'selected' : ''}`}
                        onClick={() => setSelected(b.id)}
                        key={b.id}
                      >
                        <div className="build-row-top">
                          <span className="repo">{repoName(b.repository)}</span>
                          <Badge
                            status={b.comparison.status === 'regression' ? 'regression' : b.status}
                          />
                        </div>
                        <div className="build-row-meta">
                          <span>
                            <GitBranch size={13} />
                            <code>{b.commit.length > 14 ? b.commit.slice(0, 12) : b.commit}</code>
                          </span>
                          <span>{b.jobs.length} configs</span>
                          <ChevronRight size={14} />
                        </div>
                        <div className="build-row-bottom">
                          <code>{b.id.slice(0, 10)}</code>
                          <span>{when(b.created_at)}</span>
                        </div>
                      </button>
                    ))}
                    {filtered.length === 0 && (
                      <div className="empty">
                        <Layers size={25} />
                        <h3>
                          {data.builds.length
                            ? 'No matching builds'
                            : 'Your first build starts here'}
                        </h3>
                        <p>
                          {data.builds.length
                            ? 'Adjust your search or status filter.'
                            : 'Submit a CMake repository to run its compiler matrix.'}
                        </p>
                        {!data.builds.length && (
                          <button onClick={() => setForm(true)}>
                            Submit a build <ChevronRight size={14} />
                          </button>
                        )}
                      </div>
                    )}
                  </div>
                </section>
                {build ? (
                  <BuildPanel
                    build={build}
                    threshold={data.regression_threshold}
                    busy={busy}
                    action={action}
                    onError={setError}
                  />
                ) : (
                  <section className="panel detail-empty">
                    <Terminal size={32} />
                    <h2>Execution, with evidence.</h2>
                    <p>
                      Select a build to inspect its matrix, live logs, regression comparison, and
                      artifacts.
                    </p>
                    <div className="pipeline">
                      {['Clone', 'Build', 'Test', 'Compare', 'Publish'].map((x, i) => (
                        <React.Fragment key={x}>
                          <span>{x}</span>
                          {i < 4 && <ChevronRight size={14} />}
                        </React.Fragment>
                      ))}
                    </div>
                  </section>
                )}
              </div>
              <div className="bottom-layout">
                <Performance builds={data.builds} />
                <section className="panel failures">
                  <div className="panel-heading">
                    <h2>Failure clusters</h2>
                    <span className="muted">{clusters.size} signatures</span>
                  </div>
                  {clusters.size ? (
                    <div className="cluster-list">
                      {Array.from(clusters).map(([sig, list]) => (
                        <button key={sig} onClick={() => setSelected(list[0].build_id)}>
                          <span className="cluster-count">{list.length}</span>
                          <span>
                            <code>{sig}</code>
                            <small>
                              {new Set(list.map((j) => j.build_id)).size} builds · {list.length}{' '}
                              configuration failures
                            </small>
                          </span>
                          <ChevronRight size={15} />
                        </button>
                      ))}
                    </div>
                  ) : (
                    <div className="small-empty">
                      <Check size={20} />
                      <span>
                        No failures to investigate.
                        <small>Related failures are grouped by normalized signature.</small>
                      </span>
                    </div>
                  )}
                </section>
              </div>
            </>
          )}
          {page === 'workers' && (
            <section className="panel">
              <div className="panel-heading">
                <h2>Registered workers</h2>
                <span className="muted">Heartbeat timeout · 20s</span>
              </div>
              <div className="table-scroll">
                <table>
                  <thead>
                    <tr>
                      <th>Worker</th>
                      <th>Health</th>
                      <th>Compilers</th>
                      <th>Capacity</th>
                      <th>Platform</th>
                      <th>Last heartbeat</th>
                    </tr>
                  </thead>
                  <tbody>
                    {data.workers.map((w) => (
                      <tr key={w.id}>
                        <td>
                          <strong>{w.id}</strong>
                          {w.verilator && <small>Verilator enabled</small>}
                        </td>
                        <td>
                          <Badge status={w.healthy ? 'online' : 'offline'} />
                        </td>
                        <td>{w.compilers.join(', ')}</td>
                        <td>
                          {w.active_jobs} / {w.slots} slots
                        </td>
                        <td>{w.platform}</td>
                        <td>{when(w.last_seen)}</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
              {!data.workers.length && (
                <div className="empty">
                  <Server size={28} />
                  <h3>No workers registered</h3>
                  <p>Start a worker with CONTROL_ADDR pointing to this control plane.</p>
                </div>
              )}
            </section>
          )}
          {page === 'releases' && (
            <section className="panel">
              <div className="panel-heading">
                <h2>Published versions</h2>
                <span className="muted">Verified SHA-256 artifacts</span>
              </div>
              {data.releases.length ? (
                <div className="table-scroll">
                  <table>
                    <thead>
                      <tr>
                        <th>Version</th>
                        <th>Repository</th>
                        <th>Commit</th>
                        <th>Promoted</th>
                        <th>Build</th>
                      </tr>
                    </thead>
                    <tbody>
                      {[...data.releases].reverse().map((r) => {
                        const b = data.builds.find((x) => x.id === r.build_id);
                        return (
                          <tr key={r.id}>
                            <td>
                              <strong>{r.version}</strong>
                            </td>
                            <td>{b ? repoName(b.repository) : 'Older build'}</td>
                            <td>
                              <code>{b?.commit.slice(0, 12) || '—'}</code>
                            </td>
                            <td>{when(r.created_at)}</td>
                            <td>
                              <button
                                onClick={() => {
                                  setSelected(r.build_id);
                                  setPage('builds');
                                  if (!b)
                                    void api<Build>(`/api/builds/${r.build_id}`)
                                      .then((d) =>
                                        setData((old) => ({ ...old, builds: [...old.builds, d] })),
                                      )
                                      .catch((e) => setError(e.message));
                                }}
                              >
                                View artifacts <ArrowUpRight size={14} />
                              </button>
                            </td>
                          </tr>
                        );
                      })}
                    </tbody>
                  </table>
                </div>
              ) : (
                <div className="empty">
                  <ArrowUpRight size={28} />
                  <h3>No releases published</h3>
                  <p>Open a passing build and promote its verified artifacts to a version.</p>
                </div>
              )}
            </section>
          )}
        </main>
        <footer>
          <span>RegressGrid</span>
          <span>Live worker data · refreshes every 2 seconds</span>
          <a href="/healthz" target="_blank" rel="noreferrer">
            Service health <ArrowUpRight size={12} />
          </a>
        </footer>
      </div>
    </>
  );
}

function SubmitForm({
  data,
  onClose,
  onSubmit,
  busy,
}: {
  data: GridState;
  onClose: () => void;
  onSubmit: (body: unknown) => Promise<void>;
  busy: boolean;
}) {
  const [repo, setRepo] = useState(''),
    [commit, setCommit] = useState('main'),
    [configs, setConfigs] = useState(['gcc:Debug', 'gcc:Release', 'clang:Debug', 'clang:Release']),
    [baseline, setBaseline] = useState(''),
    [plugin, setPlugin] = useState('cmake'),
    [timeout, setTimeoutValue] = useState(600),
    [error, setError] = useState('');
  return (
    <section className="panel submission">
      <div className="panel-heading">
        <h2>Submit a build</h2>
        <button aria-label="Close submission" onClick={onClose}>
          <X size={18} />
        </button>
      </div>
      <form
        onSubmit={async (e) => {
          e.preventDefault();
          setError('');
          try {
            if (!configs.length) throw new Error('Select at least one configuration.');
            await onSubmit({
              repository: repo,
              commit,
              configurations: configs.map((c) => ({
                compiler: c.split(':')[0],
                build_type: c.split(':')[1],
              })),
              baseline_id: baseline,
              plugin,
              timeout_seconds: timeout,
            });
          } catch (e) {
            setError((e as Error).message);
          }
        }}
      >
        <div className="form-grid">
          <label>
            Repository
            <input
              required
              placeholder="https://github.com/team/project.git"
              value={repo}
              onChange={(e) => {
                setRepo(e.target.value);
                setBaseline('');
              }}
            />
          </label>
          <label>
            Commit or ref
            <input
              required
              placeholder="Commit SHA, branch, or tag"
              value={commit}
              onChange={(e) => setCommit(e.target.value)}
            />
          </label>
          <label>
            Baseline
            <select value={baseline} onChange={(e) => setBaseline(e.target.value)}>
              <option value="">No baseline (first run)</option>
              {data.builds
                .filter((b) => b.status === 'passed' && b.repository === repo)
                .map((b) => (
                  <option key={b.id} value={b.id}>
                    {b.commit.slice(0, 12)} · {when(b.created_at)}
                  </option>
                ))}
            </select>
          </label>
          <label>
            Execution plugin
            <select value={plugin} onChange={(e) => setPlugin(e.target.value)}>
              <option value="cmake">CMake / CTest</option>
              <option value="verilator">Verilator / CMake</option>
            </select>
          </label>
        </div>
        <div className="form-bottom">
          <fieldset>
            <legend>Build matrix</legend>
            <div className="checks">
              {['gcc:Debug', 'gcc:Release', 'clang:Debug', 'clang:Release'].map((c) => (
                <label key={c}>
                  <input
                    type="checkbox"
                    checked={configs.includes(c)}
                    onChange={() =>
                      setConfigs((old) =>
                        old.includes(c) ? old.filter((x) => x !== c) : [...old, c],
                      )
                    }
                  />
                  {c.replace(':', ' / ')}
                </label>
              ))}
            </div>
          </fieldset>
          <label className="timeout">
            Timeout (seconds)
            <input
              type="number"
              min={1}
              max={7200}
              required
              value={timeout}
              onChange={(e) => setTimeoutValue(Number(e.target.value))}
            />
          </label>
          <button className="primary" disabled={busy} type="submit">
            {busy ? <LoaderCircle size={16} className="spin" /> : <Plus size={16} />}Queue build
          </button>
        </div>
        {error && (
          <p role="alert" className="form-error">
            {error}
          </p>
        )}
        <p className="form-help">
          The repository must register CTest tests and CMake install rules. Configuration jobs run
          on compatible workers.
        </p>
      </form>
    </section>
  );
}

function BuildPanel({
  build,
  threshold,
  busy,
  action,
  onError,
}: {
  build: Build;
  threshold: number;
  busy: boolean;
  action: (path: string, body?: unknown) => Promise<void>;
  onError: (s: string) => void;
}) {
  const [tab, setTab] = useState('logs'),
    [jobID, setJobID] = useState(build.jobs[0]?.id || ''),
    [version, setVersion] = useState(''),
    [waveform, setWaveform] = useState(''),
    [promoting, setPromoting] = useState(false);
  useEffect(() => {
    setJobID(build.jobs[0]?.id || '');
    setPromoting(false);
    setVersion('');
    setWaveform('');
  }, [build.id]);
  const job = build.jobs.find((j) => j.id === jobID) || build.jobs[0];
  useEffect(() => setWaveform(''), [jobID]);
  const promotion =
    build.status === 'passed' &&
    ['passed', 'no_baseline'].includes(build.comparison.status) &&
    build.jobs.every((j) => j.artifacts.length > 0);
  return (
    <section className="panel build-detail">
      <div className="detail-heading">
        <div>
          <div className="detail-title">
            <h2>{repoName(build.repository)}</h2>
            <Badge status={build.status} />
            {build.comparison.status === 'regression' && <Badge status="regression" />}
          </div>
          <p>
            <code>{build.id}</code>
            <span>·</span>
            <GitBranch size={13} />
            <code>{build.commit}</code>
          </p>
        </div>
        <div className="detail-actions">
          {!done(build.status) ? (
            <button
              disabled={busy}
              className="danger-text"
              onClick={() => void action(`/api/builds/${build.id}/cancel`)}
            >
              <Square size={13} />
              Cancel
            </button>
          ) : (
            <button disabled={busy} onClick={() => void action(`/api/builds/${build.id}/retry`)}>
              <RefreshCw size={13} />
              Rerun
            </button>
          )}
          <button
            className="promote"
            disabled={!promotion || busy}
            onClick={() => setPromoting((v) => !v)}
            title={
              promotion
                ? 'Publish verified artifacts'
                : 'Requires a passed matrix and baseline comparison'
            }
          >
            <ArrowUpRight size={14} />
            Promote
          </button>
        </div>
      </div>
      {promoting && (
        <form
          className="promote-form"
          onSubmit={(e) => {
            e.preventDefault();
            void action(`/api/builds/${build.id}/promote`, { version }).then(() =>
              setPromoting(false),
            );
          }}
        >
          <label>
            Release version
            <input
              required
              placeholder="v1.0.0"
              value={version}
              onChange={(e) => setVersion(e.target.value)}
              pattern="[a-zA-Z0-9][a-zA-Z0-9_.-]{0,63}"
            />
          </label>
          <button className="primary" disabled={busy}>
            Publish release
          </button>
        </form>
      )}
      <div className="matrix-heading">
        <h3>Configuration matrix</h3>
        <span>
          {build.jobs.filter((j) => j.status === 'passed').length} / {build.jobs.length} passed
        </span>
      </div>
      <div className="table-scroll">
        <table className="matrix">
          <thead>
            <tr>
              <th>Configuration</th>
              <th>Status</th>
              <th>Build</th>
              <th>Tests</th>
              <th>Peak memory</th>
            </tr>
          </thead>
          <tbody>
            {build.jobs.map((j) => (
              <tr key={j.id} className={job?.id === j.id ? 'selected' : ''}>
                <td>
                  <button onClick={() => setJobID(j.id)}>
                    <strong>{j.compiler}</strong>
                    <span>{j.build_type}</span>
                  </button>
                </td>
                <td>
                  <Badge status={j.status} />
                </td>
                <td>{j.metrics.build_seconds ? duration(j.metrics.build_seconds) : '—'}</td>
                <td>
                  {done(j.status) && j.metrics.tests_passed + j.metrics.tests_failed > 0 ? (
                    <span className={j.metrics.tests_failed ? 'test-failed' : ''}>
                      {j.metrics.tests_passed} / {j.metrics.tests_passed + j.metrics.tests_failed}
                    </span>
                  ) : (
                    '—'
                  )}
                </td>
                <td>{j.metrics.peak_memory_bytes ? bytes(j.metrics.peak_memory_bytes) : '—'}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      {job && (
        <div className="job-measurements">
          <span>
            Warnings <strong>{job.metrics.warnings}</strong>
          </span>
          <span>
            CPU <strong>{duration(job.metrics.cpu_seconds)}</strong>
          </span>
          <span>
            Tests <strong>{duration(job.metrics.test_seconds)}</strong>
          </span>
          <span>
            Installed size{' '}
            <strong>{job.metrics.binary_bytes ? bytes(job.metrics.binary_bytes) : '—'}</strong>
          </span>
          {job.metrics.latency_ms > 0 && (
            <span>
              Latency <strong>{job.metrics.latency_ms.toFixed(2)} ms</strong>
            </span>
          )}
        </div>
      )}
      <div className="detail-tabs" role="tablist" aria-label="Build details">
        {[
          { id: 'logs', label: 'Live logs', icon: Terminal },
          { id: 'comparison', label: 'Baseline comparison', icon: GitBranch },
          { id: 'artifacts', label: 'Artifacts', icon: Download },
        ].map((t) => (
          <button
            key={t.id}
            role="tab"
            aria-selected={tab === t.id}
            onClick={() => setTab(t.id)}
            className={tab === t.id ? 'active' : ''}
          >
            <t.icon size={14} />
            {t.label}
          </button>
        ))}
      </div>
      {tab === 'logs' && job && <Logs key={job.id} job={job} />}
      {tab === 'comparison' && (
        <div className="comparison">
          <div className="comparison-status">
            <span>Regression gate</span>
            <Badge status={build.comparison.status} />
          </div>
          <p className="muted">
            {build.baseline_id
              ? `Baseline ${build.baseline_id} · threshold ${threshold}%`
              : 'Choose a passed baseline when submitting a build to compare results.'}
          </p>
          {build.comparison.reason && <p className="form-error">{build.comparison.reason}</p>}
          {build.comparison.changes.length > 0 && (
            <div className="table-scroll">
              <table>
                <thead>
                  <tr>
                    <th>Configuration / metric</th>
                    <th>Baseline</th>
                    <th>Candidate</th>
                    <th>Change</th>
                  </tr>
                </thead>
                <tbody>
                  {build.comparison.changes
                    .filter((c) => c.job_id === job?.id)
                    .map((c) => (
                      <tr key={c.metric}>
                        <td>{c.metric.replaceAll('_', ' ')}</td>
                        <td>{c.baseline.toFixed(2)}</td>
                        <td>{c.candidate.toFixed(2)}</td>
                        <td className={c.regression ? 'test-failed' : 'positive'}>
                          {c.baseline === 0
                            ? 'New'
                            : `${c.percent > 0 ? '+' : ''}${c.percent.toFixed(1)}%`}
                          {c.regression && ' · regression'}
                        </td>
                      </tr>
                    ))}
                </tbody>
              </table>
            </div>
          )}
          <p className="form-help">Select a configuration row to inspect its measurements.</p>
        </div>
      )}
      {tab === 'artifacts' && (
        <div className="artifacts">
          {waveform && job && (
            <Waveform job={job.id} name={waveform} close={() => setWaveform('')} />
          )}
          {job?.artifacts.length ? (
            job.artifacts.map((a) => (
              <div key={a.name} className="artifact">
                <Download size={17} />
                <div>
                  <strong>{a.name}</strong>
                  <code title={a.sha256}>SHA-256 {a.sha256.slice(0, 20)}…</code>
                </div>
                <span>{bytes(a.size)}</span>
                {a.name.endsWith('.vcd') && (
                  <button onClick={() => setWaveform(a.name)}>View waveform</button>
                )}
                <button
                  disabled={!['passed', 'failed'].includes(job.status)}
                  onClick={() => void download(job.id, a.name).catch((e) => onError(e.message))}
                >
                  Download
                </button>
              </div>
            ))
          ) : (
            <div className="small-empty">
              <Download size={20} />
              <span>
                No artifacts yet.
                <small>Passing jobs upload their installed binaries and test reports.</small>
              </span>
            </div>
          )}
        </div>
      )}
      <div className="detail-footer">
        <span>{job?.worker_id || 'Awaiting compatible worker'}</span>
        <span>
          Attempt {job?.attempt || 0} · {build.plugin}
        </span>
        <span>{when(build.created_at)}</span>
      </div>
    </section>
  );
}

function Logs({ job }: { job: Job }) {
  const [log, setLog] = useState(''),
    [error, setError] = useState(''),
    [following, setFollowing] = useState(true);
  useEffect(() => {
    const controller = new AbortController();
    setLog('');
    setError('');
    async function stream() {
      try {
        const r = await fetch(`/api/jobs/${job.id}/logs?stream=1`, {
          headers: headers(),
          signal: controller.signal,
        });
        if (!r.ok || !r.body) throw new Error('Could not stream logs. Check your connection.');
        const reader = r.body.getReader(),
          decoder = new TextDecoder();
        let pending = '';
        while (true) {
          const { done, value } = await reader.read();
          if (done) break;
          pending += decoder.decode(value, { stream: true });
          let end;
          while ((end = pending.indexOf('\n\n')) !== -1) {
            const block = pending.slice(0, end);
            pending = pending.slice(end + 2);
            if (block.startsWith('data: ')) {
              const text = JSON.parse(block.slice(6));
              setLog((old) => (old + text).slice(-250000));
            }
          }
        }
      } catch (e) {
        if (!controller.signal.aborted) setError((e as Error).message);
      }
    }
    void stream();
    return () => controller.abort();
  }, [job.id]);
  useEffect(() => {
    if (following) {
      const el = document.getElementById('log-output');
      if (el) el.scrollTop = el.scrollHeight;
    }
  }, [log, following]);
  return (
    <div className="log-view">
      <div className="console-bar">
        <span>
          <Terminal size={13} />
          {job.compiler} / {job.build_type}
        </span>
        <label>
          <input
            type="checkbox"
            checked={following}
            onChange={(e) => setFollowing(e.target.checked)}
          />
          Follow output
        </label>
        <span className="console-state">
          {!done(job.status) && job.status === 'running' ? (
            <>
              <span className="live-dot" />
              Streaming
            </>
          ) : (
            'Captured output'
          )}
        </span>
      </div>
      <pre id="log-output" tabIndex={0} aria-label="Build log output">
        {error || log || 'Waiting for worker output…'}
      </pre>
      {job.error && (
        <div className="job-error">
          <strong>{job.status === 'cancelled' ? 'Execution cancelled' : 'Failure summary'}</strong>
          <code>{job.error}</code>
        </div>
      )}
    </div>
  );
}

function Performance({ builds }: { builds: Build[] }) {
  const points = [...builds]
    .reverse()
    .map((b) => ({ b, values: b.jobs.map((j) => j.metrics.latency_ms).filter((n) => n > 0) }))
    .filter((p) => p.values.length)
    .slice(-12)
    .map((p) => ({
      label: p.b.commit.slice(0, 7),
      value: p.values.reduce((a, b) => a + b, 0) / p.values.length,
    }));
  const max = Math.max(...points.map((p) => p.value), 1) * 1.25;
  return (
    <section className="panel performance">
      <div className="panel-heading">
        <h2>Benchmark latency</h2>
        <span className="muted">Mean across configurations · ms</span>
      </div>
      {points.length ? (
        <>
          <svg viewBox="0 0 600 155" role="img" aria-label="Measured benchmark latency by commit">
            {[0, 0.5, 1].map((f) => (
              <g key={f}>
                <line x1="40" x2="580" y1={120 - f * 95} y2={120 - f * 95} stroke="#e0e7e4" />
                <text x="30" y={124 - f * 95} textAnchor="end">
                  {(max * f).toFixed(0)}
                </text>
              </g>
            ))}
            <polyline
              fill="none"
              stroke="#18765d"
              strokeWidth="2.5"
              points={points
                .map(
                  (p, i) =>
                    `${55 + i * (510 / Math.max(points.length - 1, 1))},${120 - (p.value / max) * 95}`,
                )
                .join(' ')}
            />
            {points.map((p, i) => (
              <g key={i}>
                <circle
                  cx={55 + i * (510 / Math.max(points.length - 1, 1))}
                  cy={120 - (p.value / max) * 95}
                  r="4"
                  fill="#18765d"
                >
                  <title>
                    {p.label}: {p.value.toFixed(2)} ms
                  </title>
                </circle>
                <text
                  x={55 + i * (510 / Math.max(points.length - 1, 1))}
                  y="144"
                  textAnchor="middle"
                >
                  {points.length > 6 && i % 2 ? '' : p.label}
                </text>
              </g>
            ))}
          </svg>
          <p className="chart-note">
            {points.length} measured builds · latest {points.at(-1)!.value.toFixed(2)} ms
          </p>
        </>
      ) : (
        <div className="small-empty">
          <GitBranch size={20} />
          <span>
            Awaiting benchmark measurements.
            <small>Tests can write latency_ms and throughput to regressgrid-metrics.json.</small>
          </span>
        </div>
      )}
    </section>
  );
}
createRoot(document.getElementById('root')!).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>,
);
