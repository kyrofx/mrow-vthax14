let state = {tracks: [], plays: [], feedback: []}, setlist = [], busy = false, revision = null;
let pendingPlay = null;
let planBasis = null;
const $ = id => document.getElementById(id);
$('session').value = new URLSearchParams(location.search).get('session') || 'default';
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
  message('Working…');
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
  if (track.model_reason) {
    const reason = document.createElement('p'); reason.textContent = track.model_reason;
    details.append(reason);
  }
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
  const view = await api('agent/view');
  const nextRevision = JSON.stringify([next, view]);
  if (onlyChanged && nextRevision === revision) { message('Ready.'); return; }
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
  // Active plans belong to the session, including plans made inside Mixxx.
  const picks = await api('agent', view.plan ? {} : {count: 5, options: options()});
  renderPlan(picks.plan);
  $('agentNote').textContent = (picks.source === 'model' ? `Model · ${picks.model}` : 'Local scoring') +
    ' · ' + picks.summary + (picks.model_error ? ' ' + picks.model_error : '');
  $('modelState').textContent = view.settings.connected ? view.settings.next_model : 'Local mode';
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
  message('Ready. Crowd ratings update the upcoming plan.');
}
$('refresh').onclick = () => action(async () => { await api('agent', {retry: true, count: 5}); await refresh(); });
$('session').onchange = () => action(async () => { clearSetlist(); await refresh(); message('Session loaded.'); });
for (const id of ['direction', 'maxDelta', 'targetBpm', 'halfDouble', 'harmonic']) {
  $(id).onchange = () => action(async () => {
    await api('agent', {action: setlist.length ? 'adjust' : 'next', options: options(), count: Number($('count').value)});
    await refresh(); message('Scoring controls applied.');
  });
}
$('import').onchange = () => action(async () => {
  const file = $('import').files[0]; if (!file) return;
  const result = await api('library', {tracks: JSON.parse(await file.text())});
  clearSetlist(); await refresh(); message(`Imported ${result.imported} tracks.`); $('import').value = '';
});
function renderPlan(plan) {
  clearSetlist();
  planBasis = plan?.basis || null;
  if (!plan) return;
  setlist = plan.tracks;
  $('direction').value = plan.options.direction;
  $('maxDelta').value = plan.options.max_bpm_delta;
  $('targetBpm').value = plan.options.target_bpm ?? '';
  $('halfDouble').checked = plan.options.allow_half_double;
  $('harmonic').checked = plan.options.harmonic_only;
  $('count').value = plan.requested;
  for (const track of setlist) {
    const li = document.createElement('li'), title = document.createElement('p');
    title.textContent = label(track); li.append(title, explanation(track)); $('setlist').append(li);
  }
  $('setlistNote').textContent = `${plan.returned} of ${plan.requested} upcoming songs. ${plan.summary} ${plan.notes.join(' ')} ` +
    (plan.model_error || '') + ' Updated after plays and crowd feedback. Playback stays under your control.';
}
async function plan(direction) {
  if (direction) $('direction').value = direction;
  await api('agent', {action: 'generate', count: Number($('count').value), options: options()});
  await refresh();
}
$('generate').onclick = () => action(() => plan());
for (const [id, direction] of Object.entries({adjust: 'auto', build: 'up', hold: 'steady', ease: 'down'})) {
  $(id).onclick = () => action(() => plan(direction));
}
$('clearPlan').onclick = () => action(async () => { await api('agent', {action: 'clear'}); await refresh(); });
$('export').onclick = () => action(async () => {
  const result = await api('agent/export', {basis: planBasis});
  const url = URL.createObjectURL(new Blob([result.content], {type: 'audio/x-mpegurl'}));
  const a = document.createElement('a'); a.href = url; a.download = result.filename; a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000); message('Setlist exported.');
});
$('models').onclick = () => action(async () => {
  const result = await api('agent/models');
  $('modelCatalog').replaceChildren();
  for (const model of result.models) {
    const option = document.createElement('option'); option.value = model.id; option.label = model.name;
    $('modelCatalog').append(option);
  }
  $('modelNote').textContent = `${result.models.length} models loaded. Select or type a model ID. Pricing varies by model.`;
  message('Model catalog loaded.');
});
$('connect').onclick = () => action(async () => {
  const key = $('apiKey').value; $('apiKey').value = '';
  await api('agent/settings', {api_key: key, next_model: $('nextModel').value, plan_model: $('planModel').value});
  await refresh(); message('Models applied. See advice status for connection results.');
});
$('disconnect').onclick = () => action(async () => {
  $('apiKey').value = '';
  await api('agent/settings', {disconnect: true}); await refresh(); message('Disconnected. Local scoring is active.');
});
action(async () => {
  const config = await api('agent/settings');
  if (config.connected) {
    $('nextModel').value = config.next_model; $('planModel').value = config.plan_model;
    $('apiKey').placeholder = 'Leave blank to keep runtime key';
  }
  await refresh();
});
setInterval(() => { if (!busy && !document.hidden && !['INPUT', 'SELECT'].includes(document.activeElement.tagName)) action(() => refresh(true)); }, 5000);
