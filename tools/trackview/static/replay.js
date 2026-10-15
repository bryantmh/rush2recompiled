// Trackview's race replay: plays a recording of src/race_record.cpp (RUSH2_RACE_RECORD) over the track, with play,
// pause, fast forward, rewind and a scrubber, a car table and the race's events (wrecks, stuck spells, laps,
// finishes). Recordings come from the server's /runs (tools/trackview/runs.py). See docs/custom_tracks.md#race-replays
import * as THREE from 'three';

const FT = 3.2808399;
const MPH = 60 / 88;                       // ft/s -> mph
const CAR_COLORS = ['#e8e8e8', '#ff5d5d', '#ffb34d', '#ffe14d', '#5ddb6b', '#4dc3ff', '#9d7bff', '#ff7bd3'];
const SPEEDS = [-32, -16, -8, -4, -2, -1, 0.5, 1, 2, 4, 8, 16, 32];
const TRAIL_SECONDS = 8;
const TELEPORT = 20;                       // m between two steps of a car: a jump (respawn), not driving
const STUCK_SPEED = 15, STUCK_SECONDS = 3;  // ft/s, s: a car this slow for this long (racing, not wrecked) is stuck

export function parseRecording(buf) {
  const dv = new DataView(buf);
  if (buf.byteLength < 12 || String.fromCharCode(...new Uint8Array(buf, 0, 4)) !== 'R2RC') throw new Error('not a race recording');
  const hl = dv.getUint32(8, true);
  const header = JSON.parse(new TextDecoder().decode(new Uint8Array(buf, 12, hl)));
  const nf = header.fields.length, stride = 2 + header.cars * nf;
  const ticks = Math.floor((buf.byteLength - 12 - hl) / (stride * 4));
  const f = new Float32Array(buf.slice(12 + hl, 12 + hl + ticks * stride * 4));
  const idx = Object.fromEntries(header.fields.map((n, i) => [n, i]));
  const t0 = ticks ? f[1] : 0;
  const time = new Float64Array(ticks);
  for (let t = 0; t < ticks; t++) time[t] = f[t * stride + 1] - t0;
  const get = (t, c, name) => f[t * stride + 2 + c * nf + idx[name]];
  // Each car's own samples: drones are stepped every other physics tick, so between steps a car's pose holds. A
  // sample is fresh where the car's pose changes, and the car state publishes a step's pose one tick after its step
  // clock ('step', car +0x714) moves, so a fresh sample takes the step time recorded with it. Cars then move smoothly
  // between their steps instead of in stairs.
  const keys = [], keyTime = [];
  const moved = (t, c) => ['x', 'y', 'z', 'fx', 'fz', 'uy'].some(n => get(t, c, n) !== get(t - 1, c, n));
  for (let c = 0; c < header.cars; c++) {
    const k = [], kt = [];
    for (let t = 0; t < ticks; t++) {
      if (t !== 0 && t !== ticks - 1 && !moved(t, c)) continue;
      let at = 'step' in idx && t > 0 ? get(t, c, 'step') - t0 : time[t];
      if (!(at >= (kt.length ? kt[kt.length - 1] : 0) && at <= time[t])) at = time[t];   // before its first step, or odd
      if (kt.length && at <= kt[kt.length - 1]) continue;
      k.push(t); kt.push(at);
    }
    keys.push(Int32Array.from(k)); keyTime.push(Float64Array.from(kt));
  }
  return { header, ticks, time, get, keys, keyTime, duration: ticks ? time[ticks - 1] : 0 };
}

// What happened in the race: [{t, car, kind, text}]
function findEvents(rec) {
  const ev = [], n = rec.header.cars;
  for (let c = 0; c < n; c++) {
    const human = rec.header.kinds[c] !== 1;
    let slowSince = -1, stuck = false, lapStart = 0;
    for (let t = 1; t < rec.ticks; t++) {
      const time = rec.time[t];
      const g = name => rec.get(t, c, name), p = name => rec.get(t - 1, c, name);
      if (g('wreck') && !p('wreck')) ev.push({ t: time, car: c, kind: 'wreck', text: 'wrecked' });
      if (g('respawn') >= 0 && p('respawn') < 0) ev.push({ t: time, car: c, kind: 'respawn', text: 'put back on the road' });
      if (g('laps') > p('laps')) {
        ev.push({ t: time, car: c, kind: 'lap', text: `lap ${g('laps')} in ${fmt(time - lapStart)}` });
        lapStart = time;
      }
      if (g('finished') && !p('finished')) ev.push({ t: time, car: c, kind: 'finish', text: `finished ${ordinal(g('place') + 1)}` });
      if (human) continue;
      const racing = time > 5 && !g('finished') && !g('wreck') && g('respawn') < 0;
      if (racing && Math.abs(g('speed')) < STUCK_SPEED) {
        if (slowSince < 0) slowSince = time;
        if (!stuck && time - slowSince >= STUCK_SECONDS) {
          stuck = true;
          ev.push({ t: slowSince, car: c, kind: 'stuck', text: `stuck (under ${Math.round(STUCK_SPEED * MPH)} mph for ${STUCK_SECONDS} s+)` });
        }
      } else { slowSince = -1; stuck = false; }
    }
  }
  return ev.sort((a, b) => a.t - b.t);
}

function fmt(s) {
  const neg = s < 0; s = Math.abs(s);
  return (neg ? '-' : '') + Math.floor(s / 60) + ':' + (s % 60).toFixed(1).padStart(4, '0');
}
function ordinal(n) { const s = ['th', 'st', 'nd', 'rd'], v = n % 100; return n + (s[(v - 20) % 10] || s[v] || s[0]); }
const carName = (rec, c) => rec.header.kinds[c] === 1 ? `Car ${c}` : `P1 (car ${c})`;

export function createReplay({ scene, camera, controls, groups, $, store }) {
  let folder = null, rec = null, events = [];
  let playhead = 0, speed = 1, playing = false, follow = -1, lastTable = 0;
  const cars = [], trails = [];

  // ---- car models ----------------------------------------------------------------------------------------
  function buildCars() {
    for (const g of [groups.cars, groups.trails]) for (const o of [...g.children]) g.remove(o);
    cars.length = trails.length = 0;
    if (!rec) return;
    for (let c = 0; c < rec.header.cars; c++) {
      const color = CAR_COLORS[c % CAR_COLORS.length];
      const car = new THREE.Group();
      const body = new THREE.Mesh(new THREE.BoxGeometry(1.9, 1.1, 4.2), new THREE.MeshLambertMaterial({ color, transparent: true }));
      body.position.y = 0.75;
      const nose = new THREE.Mesh(new THREE.ConeGeometry(0.7, 1.4, 8).rotateX(Math.PI / 2), new THREE.MeshBasicMaterial({ color: '#111' }));
      nose.position.set(0, 1.45, 1.4);
      car.add(body, nose);
      car.add(label(rec.header.kinds[c] === 1 ? String(c) : 'P1', color));
      groups.cars.add(car);
      cars.push(car);
      const tg = new THREE.BufferGeometry();
      tg.setAttribute('position', new THREE.Float32BufferAttribute(new Float32Array(3 * 600), 3));
      const trail = new THREE.Line(tg, new THREE.LineBasicMaterial({ color, transparent: true, opacity: 0.8 }));
      trail.frustumCulled = false;
      groups.trails.add(trail);
      trails.push(trail);
    }
  }

  function label(text, color) {
    const cv = document.createElement('canvas');
    cv.width = 64; cv.height = 40;
    const ctx = cv.getContext('2d');
    ctx.fillStyle = 'rgba(15,17,22,.85)'; ctx.fillRect(0, 0, 64, 40);
    ctx.fillStyle = color; ctx.font = 'bold 30px system-ui'; ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
    ctx.fillText(text, 32, 22);
    const tex = new THREE.CanvasTexture(cv);
    tex.colorSpace = THREE.SRGBColorSpace;
    const s = new THREE.Sprite(new THREE.SpriteMaterial({ map: tex, depthTest: false, sizeAttenuation: false }));
    s.scale.set(0.032, 0.02, 1);
    s.position.y = 4;
    s.renderOrder = 11;
    return s;
  }

  // game feet -> glTF meters (the page's frame), and a direction likewise
  const pos = (t, c) => new THREE.Vector3(-rec.get(t, c, 'x') / FT, rec.get(t, c, 'y') / FT, rec.get(t, c, 'z') / FT);
  const dir = (t, c, a) => new THREE.Vector3(-rec.get(t, c, a + 'x'), rec.get(t, c, a + 'y'), rec.get(t, c, a + 'z'));

  function tickAt(time) {   // last tick at or before `time`
    let lo = 0, hi = rec.ticks - 1;
    while (lo < hi) { const m = (lo + hi + 1) >> 1; if (rec.time[m] <= time) lo = m; else hi = m - 1; }
    return lo;
  }

  const m4 = new THREE.Matrix4(), q0 = new THREE.Quaternion(), q1 = new THREE.Quaternion();
  function orientation(t, c, q) {
    const fwd = dir(t, c, 'f').normalize(), up = dir(t, c, 'u').normalize();
    const right = new THREE.Vector3().crossVectors(up, fwd).normalize();
    return q.setFromRotationMatrix(m4.makeBasis(right, up, fwd));
  }
  function keyAt(c, time) {   // last of the car's fresh samples at or before `time`
    const kt = rec.keyTime[c];
    let lo = 0, hi = kt.length - 1;
    while (lo < hi) { const m = (lo + hi + 1) >> 1; if (kt[m] <= time) lo = m; else hi = m - 1; }
    return lo;
  }
  function place() {
    if (!rec || !rec.ticks) return;
    const t = tickAt(playhead);
    for (let c = 0; c < cars.length; c++) {
      // between the car's own steps
      const i = keyAt(c, playhead), i1 = Math.min(i + 1, rec.keys[c].length - 1);
      const k = rec.keys[c][i], k1 = rec.keys[c][i1];
      const span = rec.keyTime[c][i1] - rec.keyTime[c][i];
      let a = span > 0 ? Math.min(1, Math.max(0, (playhead - rec.keyTime[c][i]) / span)) : 0;
      if (pos(k, c).distanceTo(pos(k1, c)) > TELEPORT) a = 0;   // put back on the road: no sliding there
      const p = pos(k, c).lerp(pos(k1, c), a);
      cars[c].quaternion.copy(orientation(k, c, q0).slerp(orientation(k1, c, q1), a));
      cars[c].position.copy(p);
      cars[c].children[0].material.opacity = rec.get(t, c, 'wreck') ? 0.35 : 1;
      // trail: the last TRAIL_SECONDS, every few ticks
      const arr = trails[c].geometry.attributes.position.array;
      const from = keyAt(c, Math.max(0, playhead - TRAIL_SECONDS));
      const stride = Math.max(1, Math.ceil((i - from) / 598));
      let n = 0;
      for (let j = from; j <= i && n < 599; j += stride, n++) {
        const s = rec.keys[c][j];
        arr[n * 3] = -rec.get(s, c, 'x') / FT; arr[n * 3 + 1] = rec.get(s, c, 'y') / FT + 0.4; arr[n * 3 + 2] = rec.get(s, c, 'z') / FT;
      }
      arr[n * 3] = p.x; arr[n * 3 + 1] = p.y + 0.4; arr[n * 3 + 2] = p.z; n++;
      trails[c].geometry.setDrawRange(0, n);
      trails[c].geometry.attributes.position.needsUpdate = true;
    }
    if (follow >= 0 && follow < cars.length) {
      const target = cars[follow].position;
      const off = camera.position.clone().sub(controls.target);
      controls.target.copy(target);
      camera.position.copy(target).add(off);
    }
  }

  // ---- UI ------------------------------------------------------------------------------------------------
  function table(force) {
    const now = performance.now();
    if (!rec || !rec.ticks || (!force && now - lastTable < 120)) return;
    lastTable = now;
    const t = tickAt(playhead);
    const rows = [];
    for (let c = 0; c < rec.header.cars; c++) {
      const g = name => rec.get(t, c, name);
      let state = g('finished') ? 'finished' : g('wreck') ? 'wrecked' : g('respawn') >= 0 ? 'respawning' : '';
      if (!state && rec.header.kinds[c] === 1 && playhead > 5 && Math.abs(g('speed')) < STUCK_SPEED) state = 'slow';
      rows.push({ c, place: g('place'), html: `<tr data-car="${c}" class="${follow === c ? 'sel' : ''}">
        <td><span class="sw" style="background:${CAR_COLORS[c % CAR_COLORS.length]}"></span>${rec.header.kinds[c] === 1 ? c : 'P1'}</td>
        <td>${ordinal(g('place') + 1)}</td><td>${Math.min(g('laps') + 1, rec.header.laps)}/${rec.header.laps}</td>
        <td>${Math.round(Math.abs(g('speed')) * MPH)}</td>
        <td><span class="bar thr" style="width:${Math.round(Math.max(0, g('throttle')) * 24)}px"></span><span class="bar brk" style="width:${Math.round(Math.max(0, g('brake')) * 24)}px"></span></td>
        <td class="st">${state}</td></tr>` });
    }
    rows.sort((a, b) => a.place - b.place);
    $('cars').innerHTML = `<table><tr><th>car</th><th>place</th><th>lap</th><th>mph</th><th>gas/brake</th><th></th></tr>${rows.map(r => r.html).join('')}</table>`;
    $('time').textContent = `${fmt(playhead)} / ${fmt(rec.duration)}`;
    $('scrub').value = rec.duration ? Math.round(playhead / rec.duration * 10000) : 0;
    $('speed').textContent = (playing ? '' : 'paused · ') + (speed < 0 ? 'rewind ' + -speed + 'x' : speed + 'x');
  }

  function eventsUI() {
    const marks = $('marks');
    marks.innerHTML = '';
    $('events').innerHTML = '';
    if (!rec) return;
    for (const e of events) {
      if (e.kind === 'lap') continue;
      const m = document.createElement('div');
      m.className = 'mark ' + e.kind;
      m.style.left = (e.t / rec.duration * 100) + '%';
      m.title = `${fmt(e.t)} ${carName(rec, e.car)} ${e.text}`;
      marks.append(m);
    }
    const shown = events.filter(e => e.kind !== 'respawn');
    if (!shown.length) $('events').innerHTML = '<div class="stat">No wrecks or stuck cars.</div>';
    for (const e of shown) {
      const d = document.createElement('div');
      d.className = 'ev ' + e.kind;
      d.innerHTML = `<span class="sw" style="background:${CAR_COLORS[e.car % CAR_COLORS.length]}"></span>${fmt(e.t)} ${carName(rec, e.car)} ${e.text}`;
      d.onclick = () => { seek(e.t - 2); follow = e.car; playing = false; focusCar(e.car); table(true); };
      $('events').append(d);
    }
  }

  function focusCar(c) {
    place();
    if (!cars[c]) return;
    const p = cars[c].position;
    const off = camera.position.clone().sub(controls.target);
    if (off.length() > 120) off.setLength(60);
    controls.target.copy(p);
    camera.position.copy(p).add(off);
  }

  function seek(time) {
    if (!rec) return;
    playhead = Math.min(rec.duration, Math.max(0, time));
    place();
    table(true);
  }

  function setSpeed(s) { speed = s; playing = s !== 0; table(true); }
  function faster(sign) {   // sign 1: forward faster, -1: rewind faster
    if (sign > 0) setSpeed(speed <= 0 ? 1 : Math.min(32, speed * 2));
    else setSpeed(speed >= 0 ? -1 : Math.max(-32, speed * 2));
  }

  $('cars').addEventListener('click', e => {
    const tr = e.target.closest('tr[data-car]');
    if (!tr) return;
    const c = +tr.dataset.car;
    follow = follow === c ? -1 : c;
    if (follow >= 0) focusCar(c);
    table(true);
  });
  $('play').onclick = () => { playing = !playing; if (playing && speed === 0) speed = 1; table(true); };
  $('rew').onclick = () => faster(-1);
  $('ff').onclick = () => faster(1);
  $('tostart').onclick = () => seek(0);
  $('toend').onclick = () => seek(rec ? rec.duration : 0);
  $('unfollow').onclick = () => { follow = -1; table(true); };
  $('scrub').oninput = e => { if (rec) seek(e.target.value / 10000 * rec.duration); };
  $('runai').onclick = async () => {
    const backward = $('dir').value === 'backward' ? '1' : '0';
    const r = await fetch(`/run_start?folder=${encodeURIComponent(folder)}&backward=${backward}`, { method: 'POST' }).then(r => r.json());
    if (r.error) { $('runstatus').textContent = r.error; return; }
    poll();
  };
  window.addEventListener('keydown', e => {
    if (!rec || e.target.tagName === 'INPUT' || e.target.tagName === 'SELECT') return;
    const k = e.key.toLowerCase();
    if (k === ' ') { $('play').onclick(); e.preventDefault(); }
    else if (k === 'l') faster(1);
    else if (k === 'j') faster(-1);
    else if (k === 'k') { playing = false; table(true); }
    else if (e.key === 'ArrowRight') seek(playhead + (e.shiftKey ? 10 : 2));
    else if (e.key === 'ArrowLeft') seek(playhead - (e.shiftKey ? 10 : 2));
    else if (k === ',') seek(playhead - 1 / 60);
    else if (k === '.') seek(playhead + 1 / 60);
    else if (k === 'escape') { follow = -1; table(true); }
  });

  // ---- runs ----------------------------------------------------------------------------------------------
  // A run takes seconds, so a track keeps only its latest recording (tools/trackview/runs.py); it is shown once done.
  let pollTimer = null, shownStamp = null;

  async function poll() {
    clearTimeout(pollTimer);
    if (!folder) return;
    const f = folder;
    const r = await fetch(`/runs?folder=${encodeURIComponent(f)}`).then(r => r.json()).catch(() => null);
    if (!r || folder !== f) return;
    const st = r.status || {};
    const running = st.state === 'running';
    const here = st.folder && st.folder.replace(/\\/g, '/') === f;
    $('runai').disabled = running;
    $('runai').title = 'Race the track with computer cars in the game, headless and as fast as it runs';
    $('runstatus').textContent = running ? `Racing${here ? '' : ' ' + st.folder_name} (${Math.round(Date.now() / 1000 - st.started)} s)…` :
      here && st.state !== 'done' ? `Last run: ${st.state}.` :
      r.latest?.done ? `${(r.latest.done.ticks / 3600).toFixed(1)} min race, run in ${here && st.ended ? Math.round(st.ended - st.started) + ' s' : 'an earlier session'}.` : '';
    if (!running && r.latest && r.latest.stamp !== shownStamp) {
      shownStamp = r.latest.stamp;
      await openRun();
    }
    if (running) pollTimer = setTimeout(poll, 1000);
  }

  async function openRun() {
    const f = folder;
    const buf = await fetch(`/run_file?folder=${encodeURIComponent(f)}`).then(r => r.ok ? r.arrayBuffer() : null);
    if (!buf || folder !== f) return;
    try { rec = parseRecording(buf); } catch (e) { $('runstatus').textContent = e.message; return; }
    events = findEvents(rec);
    buildCars(); playhead = 0; playing = false; speed = 1; follow = -1;
    // ?seek=<s>&follow=<car> open the recording there (links and scripted screenshots)
    const q = new URLSearchParams(location.search);
    if (q.get('seek')) playhead = Math.min(rec.duration, +q.get('seek'));
    if (q.get('follow')) { follow = +q.get('follow'); focusCar(follow); }
    $('replay').style.display = rec.ticks ? '' : 'none';
    eventsUI();
    place();
    table(true);
  }

  return {
    setTrack(f) {
      folder = f; rec = null; events = []; follow = -1; shownStamp = null;
      buildCars();
      $('replay').style.display = 'none';
      $('cars').innerHTML = ''; $('events').innerHTML = ''; $('runstatus').textContent = '';
      poll();
    },
    frame(dt) {
      if (!rec) return;
      if (playing) {
        playhead += dt * speed;
        if (playhead >= rec.duration) { playhead = rec.duration; playing = false; }
        if (playhead <= 0) { playhead = 0; playing = false; }
      }
      place();
      table(false);
    },
  };
}
