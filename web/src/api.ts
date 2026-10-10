export function headers() {
  const token = sessionStorage.getItem('regressgrid.token');
  return {
    'Content-Type': 'application/json',
    ...(token ? { Authorization: `Bearer ${token}` } : {}),
  };
}
export async function api<T>(path: string, method = 'GET', body?: unknown): Promise<T> {
  const r = await fetch(path, {
    method,
    headers: headers(),
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  if (!r.ok) {
    const error = await r.json().catch(() => ({ error: r.statusText }));
    throw new Error(error.error || r.statusText);
  }
  return r.json();
}
export async function download(job: string, name: string) {
  const r = await fetch(`/api/jobs/${job}/artifacts/${encodeURIComponent(name)}`, {
    headers: headers(),
  });
  if (!r.ok) throw new Error('Artifact download failed. Refresh and try again.');
  const blob = await r.blob();
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = name;
  link.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
