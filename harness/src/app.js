let state = {tracks: [], plays: [], feedback: []}, setlist = [], busy = false, revision = null;
let pendingPlay = null;
const $ = id => document.getElementById(id);
const session = () => $('session').value.trim();
const options = () => ({direction: $('direction').value, max_bpm_delta: Number($('maxDelta').value),
  target_bpm: $('targetBpm').value === '' ? null : Number($('targetBpm').value),
  allow_half_double: $('halfDouble').checked, harmonic_only: $('harmonic').checked});

function message(text, error = false) {
  $('status').textContent = text;
  $('status').className = error ? 'error' : '';
}
async function api(route, data = {}) {
  const response = await fetch('/api/' + route, {method: 'POST',
    headers: {'Content-Type': 'application/json'}, body: JSON.stringify({session: session(), ...data})});
  const result = await response.json();
  if (!response.ok) throw new Error(result.error || 'Request failed');
  return result;
}
async function action(fn) {
  if (busy) return;
  busy = true;
  document.querySelectorAll('button,input,select').forEach(b => b.disabled = true);
  try { await fn(); }
  catch (error) { message(error.message, true); }
  finally {
    busy = false;
    document.querySelectorAll('button,input,select').forEach(b => b.disabled = false);
    $('export').disabled = !setlist.length;
  }
}
function button(text, fn) {
  const b = document.createElement('button');
  b.textContent = text; b.onclick = () => action(fn); return b;
}
function label(track) { return `${track.title} — ${track.artist || 'Unknown artist'} · ${track.bpm} BPM`; }
function clearSetlist() {
  setlist = []; $('setlist').replaceChildren(); $('export').disabled = true;
  $('setlistNote').textContent = 'Generate a sequence from your library and history.';
}
function explanation(track) {
  const details = document.createElement('details'), summary = document.createElement('summary');
  summary.textContent = `Score ${(track.score * 100).toFixed(1)} · ${(track.coverage * 100).toFixed(0)}% feature coverage`;
  const text = document.createElement('p'); text.textContent = track.reasons.join(' · ');
  const table = document.createElement('table');
  for (const [key, value] of Object.entries(track.components)) {
    const row = document.createElement('tr');
    for (const cell of [key.replaceAll('_', ' '), value === null ? 'Unknown (neutral)' : value.toFixed(3),
      `${(track.contributions[key] * 100).toFixed(2)} points`]) {
      const td = document.createElement('td'); td.textContent = cell; row.append(td);
    }
    table.append(row);
  }
  const note = document.createElement('p');
  note.textContent = 'Ranking score, not a probability. Contributions sum to the score. Beat/phrase alignment is not inferred.';
  details.append(summary, text, table, note); return details;
}
async function record(track) {
  const bpm = $('actualBpm').value === '' ? null : Number($('actualBpm').value);
  const key = JSON.stringify([session(), track.id, bpm]);
  if (!pendingPlay || pendingPlay.key !== key) {
    // getRandomValues also works on a trusted LAN served over plain HTTP.
    const eventId = Array.from(crypto.getRandomValues(new Uint8Array(16)), b => b.toString(16).padStart(2, '0')).join('');
    pendingPlay = {key, eventId};
  }
  await api('play', {track_id: track.id, bpm, event_id: pendingPlay.eventId});
  pendingPlay = null;
  $('actualBpm').value = ''; clearSetlist(); await refresh();
  message('Play recorded. Audio playback stays in Mixxx.');
}
async function refresh(onlyChanged = false) {
  const next = await api('state');
  const nextRevision = JSON.stringify(next);
  if (onlyChanged && nextRevision === revision) return;
  if (revision !== null && nextRevision !== revision) clearSetlist();
  revision = nextRevision; state = next;
  const byId = Object.fromEntries(state.tracks.map(t => [t.id, t]));
  const current = state.plays.at(-1);
  $('playing').textContent = current ? `${label(byId[current.track_id])} · played at ${current.bpm} BPM` : 'No song recorded in this session.';
  $('feedback').replaceChildren();
  if (current) {
    const saved = state.feedback.filter(v => v.play_id === current.id).at(-1)?.rating;
    for (const rating of ['good', 'mid', 'bad']) {
      const b = button(rating[0].toUpperCase() + rating.slice(1), async () => {
        await api('feedback', {track_id: current.track_id, play_id: current.id, rating});
        clearSetlist(); await refresh(); message('Crowd assessment saved: ' + rating);
      });
      b.setAttribute('aria-pressed', String(saved === rating)); $('feedback').append(b);
    }
  }
  $('picks').replaceChildren();
  const picks = await api('recommend', {options: options()});
  if (!picks.tracks.length) $('picks').textContent = state.tracks.length ?
    'No eligible songs match these constraints. Adjust the controls, start another session, or add tracks.' : 'Import a library to get started.';
  for (const track of picks.tracks) {
    const row = document.createElement('div'); row.className = 'track';
    const title = document.createElement('strong'); title.textContent = label(track);
    const actions = document.createElement('div'); actions.className = 'actions';
    actions.append(button('Record play', () => record(track)), button('Skip suggestion', async () => {
      await api('feedback', {track_id: track.id, rating: 'skip'});
      clearSetlist(); await refresh(); message('Suggestion skipped for this session.');
    }));
    row.append(title, explanation(track), actions); $('picks').append(row);
  }
  $('library').replaceChildren();
  for (const track of state.tracks) {
    const row = document.createElement('div'); row.className = 'track';
    const text = document.createElement('p'); text.textContent = label(track);
    row.append(text, button('Record play', () => record(track))); $('library').append(row);
  }
  $('history').replaceChildren();
  for (const play of state.plays.slice(-10).reverse()) {
    const p = document.createElement('p');
    p.textContent = label(byId[play.track_id]) + ' · ' + play.created + ' UTC'; $('history').append(p);
  }
}
$('refresh').onclick = () => action(refresh);
$('session').onchange = () => action(async () => { clearSetlist(); await refresh(); message('Session loaded.'); });
for (const id of ['direction', 'maxDelta', 'targetBpm', 'halfDouble', 'harmonic']) {
  $(id).onchange = () => action(async () => { clearSetlist(); await refresh(); message('Scoring controls applied.'); });
}
$('count').onchange = clearSetlist;
$('import').onchange = () => action(async () => {
  const file = $('import').files[0]; if (!file) return;
  const result = await api('library', {tracks: JSON.parse(await file.text())});
  clearSetlist(); await refresh(); message(`Imported ${result.imported} tracks.`); $('import').value = '';
});
$('generate').onclick = () => action(async () => {
  clearSetlist();
  const result = await api('setlist', {count: Number($('count').value), options: options()});
  setlist = result.tracks;
  for (const track of setlist) {
    const li = document.createElement('li'), title = document.createElement('p');
    title.textContent = label(track); li.append(title, explanation(track)); $('setlist').append(li);
  }
  $('setlistNote').textContent = `${result.returned} of ${result.requested} songs. ${result.notes.join(' ')} Generating does not record plays.`;
});
$('export').onclick = () => action(async () => {
  // Refresh before exporting so external playback/feedback cannot leave a stale plan.
  await refresh();
  if (!setlist.length) { message('History or library changed. Generate a fresh setlist.'); return; }
  const result = await api('export', {track_ids: setlist.map(t => t.id)});
  const url = URL.createObjectURL(new Blob([result.content], {type: 'audio/x-mpegurl'}));
  const a = document.createElement('a'); a.href = url; a.download = result.filename; a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000); message('Setlist exported.');
});
action(refresh);
setInterval(() => { if (!busy && !document.hidden && !['INPUT', 'SELECT'].includes(document.activeElement.tagName)) action(() => refresh(true)); }, 5000);
