// The playground. The published JavaScript reader fetches the index and the
// manifest of a public cozip with range requests. Every row goes into the
// table, and GeoParquet geometries go on a MapLibre map over the Natural
// Earth countries. Picking a row or a feature shows the entry, its location
// and a download of its bytes with one range request.
//
// CRS assumption: geometries are longitude and latitude, the GeoParquet
// default (OGC:CRS84).

import { read } from 'https://esm.sh/@asterisk-labs/cozip@2026.11.23';

const $ = id => document.getElementById(id);
const $form = $('play-form'), $url = $('url'), $go = $('go'), $picks = $('picks'),
      $statCount = $('stat-count'), $statTime = $('stat-time'), $statGeom = $('stat-geom'),
      $panel = $('meta-panel'), $title = $('meta-title'), $range = $('meta-range'),
      $body = $('meta-body'), $close = $('meta-close'),
      $loading = $('loading'), $loadText = $('loading-text'),
      $error = $('error'), $info = $('info'),
      $copyVsi = $('copy-vsi'), $copyName = $('copy-name'),
      $download = $('download-tile'), $dlFilename = $('download-filename'),
      $mapEmpty = $('map-empty'), $manifestSub = $('manifest-sub'),
      $tbody = $('manifest-tbody'), $count = $('manifest-count');

const GEOMETRY_KEYS = /^(geometry|geom|wkb|wkt|the_geom)$/i;

let currentRows = null, currentUrl = null, currentSelection = null;
let featureIds = new Set();
let geometries = new Map();
let loadToken = 0;

// ─── WKB parser (ISO WKB and EWKB) ─────────────────────────────────────────
function parseWKB(bytes) {
  if (!bytes || bytes.length < 5) return null;
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let pos = 0;
  function readGeom() {
    const le = view.getUint8(pos) === 1; pos += 1;
    const rawType = view.getUint32(pos, le); pos += 4;
    let baseType, hasZ = false, hasM = false;
    if ((rawType & 0xE0000000) !== 0) {
      hasZ = (rawType & 0x80000000) !== 0;
      hasM = (rawType & 0x40000000) !== 0;
      if ((rawType & 0x20000000) !== 0) pos += 4;
      baseType = rawType & 0xff;
    } else if (rawType >= 1000) {
      baseType = rawType % 1000;
      const d = Math.floor(rawType / 1000);
      hasZ = d === 1 || d === 3; hasM = d === 2 || d === 3;
    } else { baseType = rawType; }
    const skip = (hasZ ? 8 : 0) + (hasM ? 8 : 0);
    function readPoint() {
      const x = view.getFloat64(pos, le); pos += 8;
      const y = view.getFloat64(pos, le); pos += 8;
      pos += skip; return [x, y];
    }
    function readN(n) { const a = new Array(n); for (let i = 0; i < n; i++) a[i] = readPoint(); return a; }
    function readRings() {
      const n = view.getUint32(pos, le); pos += 4;
      const rings = new Array(n);
      for (let i = 0; i < n; i++) { const np = view.getUint32(pos, le); pos += 4; rings[i] = readN(np); }
      return rings;
    }
    switch (baseType) {
      case 1: return { type: 'Point', coordinates: readPoint() };
      case 2: { const n = view.getUint32(pos, le); pos += 4;
        return { type: 'LineString', coordinates: readN(n) }; }
      case 3: return { type: 'Polygon', coordinates: readRings() };
      case 4: { const n = view.getUint32(pos, le); pos += 4;
        const a = []; for (let i = 0; i < n; i++) a.push(readGeom().coordinates);
        return { type: 'MultiPoint', coordinates: a }; }
      case 5: { const n = view.getUint32(pos, le); pos += 4;
        const a = []; for (let i = 0; i < n; i++) a.push(readGeom().coordinates);
        return { type: 'MultiLineString', coordinates: a }; }
      case 6: { const n = view.getUint32(pos, le); pos += 4;
        const a = []; for (let i = 0; i < n; i++) a.push(readGeom().coordinates);
        return { type: 'MultiPolygon', coordinates: a }; }
    }
    return null;
  }
  try { return readGeom(); } catch { return null; }
}

// ─── WKT parser, the fallback for text geometry ────────────────────────────
function parseWKT(s) {
  if (typeof s !== 'string') return null;
  s = s.trim().replace(/^SRID=\d+;\s*/i, '');
  const m = s.match(/^([A-Z]+)\s*(?:Z|M|ZM)?\s*\((.*)\)\s*$/i);
  if (!m) return null;
  const type = m[1].toUpperCase(), inner = m[2];
  const pt = str => str.trim().split(/\s+/).map(Number).slice(0, 2);
  const pts = str => str.split(',').map(pt);
  const groups = str => {
    const out = []; let depth = 0, start = -1;
    for (let i = 0; i < str.length; i++) {
      const c = str[i];
      if (c === '(') { if (depth === 0) start = i + 1; depth++; }
      else if (c === ')') { depth--; if (depth === 0) out.push(str.substring(start, i)); }
    }
    return out;
  };
  switch (type) {
    case 'POINT': return { type: 'Point', coordinates: pt(inner) };
    case 'LINESTRING': return { type: 'LineString', coordinates: pts(inner) };
    case 'POLYGON': return { type: 'Polygon', coordinates: groups(inner).map(pts) };
    case 'MULTIPOINT': {
      const g = groups(inner);
      return { type: 'MultiPoint', coordinates: g.length ? g.map(pt) : pts(inner) };
    }
    case 'MULTILINESTRING': return { type: 'MultiLineString', coordinates: groups(inner).map(pts) };
    case 'MULTIPOLYGON': {
      const polys = groups(inner);
      return { type: 'MultiPolygon', coordinates: polys.map(p => groups(p).map(pts)) };
    }
  }
  return null;
}

function parseGeometryValue(v) {
  if (v == null) return null;
  if (typeof v === 'object' && !ArrayBuffer.isView(v) && !(v instanceof ArrayBuffer)
      && v.type && 'coordinates' in v) return v;
  if (typeof v === 'string') {
    const t = v.trim();
    if (t.startsWith('{')) { try { return JSON.parse(t); } catch { /* not JSON */ } }
    return parseWKT(t);
  }
  if (v instanceof Uint8Array) return parseWKB(v);
  if (ArrayBuffer.isView(v)) return parseWKB(new Uint8Array(v.buffer, v.byteOffset, v.byteLength));
  if (v instanceof ArrayBuffer) return parseWKB(new Uint8Array(v));
  return null;
}

// ─── Map (Natural Earth 1:110m countries, public domain) ───────────────────
const VIOLET = '#492ae8', HOVER = '#6d54ff', GOLD = '#f7cc09', INK = '#000000';
const selected = (on, off) => ['case', ['boolean', ['feature-state', 'selected'], false], on, off];
const lit = (on, hover, off) => ['case',
  ['boolean', ['feature-state', 'selected'], false], on,
  ['boolean', ['feature-state', 'hover'], false], hover,
  off];

let map = null;
let mapReady;

function startMap() {
  if (!window.maplibregl) {
    mapReady = Promise.resolve(false);
    showInfo('The map library did not load, so only the manifest is shown.');
    return;
  }
  map = new maplibregl.Map({
    container: 'map',
    style: {
      version: 8,
      sources: {
        'natural-earth': {
          type: 'geojson',
          data: window.NATURAL_EARTH_COUNTRIES || { type: 'FeatureCollection', features: [] },
          tolerance: 0.4,
          maxzoom: 5
        }
      },
      layers: [
        { id: 'ocean', type: 'background', paint: { 'background-color': '#eeeeeb' } },
        { id: 'countries', type: 'fill', source: 'natural-earth', paint: { 'fill-color': '#ffffff', 'fill-opacity': 1 } },
        { id: 'country-borders', type: 'line', source: 'natural-earth', paint: { 'line-color': '#cfcfcb', 'line-width': 0.7 } }
      ]
    },
    attributionControl: false,
    center: [0, 25], zoom: 1.4, pitch: 0, antialias: true,
    cooperativeGestures: window.matchMedia('(pointer: coarse)').matches
  });
  map.addControl(new maplibregl.NavigationControl({ visualizePitch: true }), 'top-left');

  mapReady = new Promise(resolve => {
    map.on('load', () => {
      // tolerance, buffer and maxzoom keep re-tiling cheap after fitBounds
      map.addSource('cozip', {
        type: 'geojson',
        data: { type: 'FeatureCollection', features: [] },
        tolerance: 0.5,
        buffer: 0,
        maxzoom: 8
      });
      const polygons = ['match', ['geometry-type'], ['Polygon', 'MultiPolygon'], true, false];
      const lines = ['match', ['geometry-type'], ['LineString', 'MultiLineString'], true, false];
      const points = ['match', ['geometry-type'], ['Point', 'MultiPoint'], true, false];
      map.addLayer({
        id: 'cozip-fill', type: 'fill', source: 'cozip', filter: polygons,
        paint: { 'fill-color': lit(GOLD, HOVER, VIOLET), 'fill-opacity': lit(0.4, 0.24, 0.12) }
      });
      map.addLayer({
        id: 'cozip-fill-outline', type: 'line', source: 'cozip', filter: polygons,
        paint: { 'line-color': selected(INK, VIOLET), 'line-width': selected(2, 1), 'line-opacity': 0.9 }
      });
      map.addLayer({
        id: 'cozip-line', type: 'line', source: 'cozip', filter: lines,
        paint: { 'line-color': lit(GOLD, HOVER, VIOLET), 'line-width': selected(3.5, 1.8) }
      });
      map.addLayer({
        id: 'cozip-circle', type: 'circle', source: 'cozip', filter: points,
        paint: {
          'circle-radius': ['interpolate', ['linear'], ['zoom'], 1, 5, 6, 9],
          'circle-color': lit(GOLD, HOVER, VIOLET),
          'circle-stroke-color': INK,
          'circle-stroke-width': selected(2, 1.2)
        }
      });

      let hoverId = null;
      const layers = ['cozip-fill', 'cozip-line', 'cozip-circle'];
      for (const layer of layers) {
        map.on('mousemove', layer, e => {
          map.getCanvas().style.cursor = 'pointer';
          if (!e.features.length) return;
          if (hoverId !== null) map.setFeatureState({ source: 'cozip', id: hoverId }, { hover: false });
          hoverId = e.features[0].id;
          map.setFeatureState({ source: 'cozip', id: hoverId }, { hover: true });
        });
        map.on('mouseleave', layer, () => {
          map.getCanvas().style.cursor = '';
          if (hoverId !== null) map.setFeatureState({ source: 'cozip', id: hoverId }, { hover: false });
          hoverId = null;
        });
        map.on('click', layer, e => {
          if (!e.features.length) return;
          select(e.features[0].id, { from: 'map' });
        });
      }
      map.on('click', e => {
        const hit = map.queryRenderedFeatures(e.point, { layers });
        if (hit.length === 0) hidePanel();
      });
      resolve(true);
    });
    map.on('error', e => console.error(e && e.error ? e.error : e));
  });
}

function setFeatures(fc) {
  if (!map || !map.getSource('cozip')) return;
  map.getSource('cozip').setData(fc);
  featureIds = new Set(fc.features.map(f => f.id));
  geometries = new Map(fc.features.map(f => [f.id, f.geometry]));
}

// ─── Geometry column ───────────────────────────────────────────────────────
function detectGeometryKey(rows) {
  if (!rows || !rows.length) return null;
  const sample = rows[0];
  for (const k of Object.keys(sample).filter(k => GEOMETRY_KEYS.test(k))) {
    if (parseGeometryValue(sample[k])) return k;
  }
  return null;
}

function rowsToGeoJSON(rows, geomKey) {
  const features = [];
  let nFailed = 0;
  const typeCounts = {};
  for (let i = 0; i < rows.length; i++) {
    const g = parseGeometryValue(rows[i][geomKey]);
    if (!g) { nFailed++; continue; }
    typeCounts[g.type] = (typeCounts[g.type] || 0) + 1;
    features.push({ type: 'Feature', id: i, geometry: g, properties: { _idx: i } });
  }
  return { fc: { type: 'FeatureCollection', features }, nFailed, typeCounts };
}

function eachPoint(coordinates, visit) {
  if (typeof coordinates[0] === 'number') visit(coordinates);
  else for (const c of coordinates) eachPoint(c, visit);
}

function geoJSONBounds(fc) {
  let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
  for (const f of fc.features) {
    eachPoint(f.geometry.coordinates, ([x, y]) => {
      if (x < minX) minX = x; if (x > maxX) maxX = x;
      if (y < minY) minY = y; if (y > maxY) maxY = y;
    });
  }
  return isFinite(minX) ? [[minX, minY], [maxX, maxY]] : null;
}

// ─── Manifest table ────────────────────────────────────────────────────────
function renderManifestTable(rows) {
  $count.textContent = rows.length.toLocaleString();
  if (!rows.length) {
    $tbody.innerHTML = '<tr class="empty-row"><td colspan="3">The manifest has no rows.</td></tr>';
    return;
  }
  $tbody.innerHTML = rows.map((row, i) => {
    const name = esc(row.name ?? `entry ${i}`);
    const offset = typeof row.offset === 'number' ? row.offset.toLocaleString() : '-';
    const size = typeof row.size === 'number' ? row.size.toLocaleString() : '-';
    return `<tr data-idx="${i}" tabindex="0"><td class="name-cell">${name}</td><td class="num">${offset}</td><td class="num">${size}</td></tr>`;
  }).join('');
}

function clearTable(message) {
  $count.textContent = '-';
  $tbody.innerHTML = `<tr class="empty-row"><td colspan="3">${esc(message)}</td></tr>`;
}

function showNoGeometry(rows) {
  const cols = Object.keys(rows[0] || {}).filter(k => !k.startsWith('cozip:'));
  const colList = cols.length
    ? ` Found columns: <code>${cols.map(esc).join('</code> <code>')}</code>.`
    : '';
  $manifestSub.innerHTML =
    'This cozip has no GeoParquet geometry, so the map is empty. ' +
    'The map expects <code>__metadata__</code> with a <code>geometry</code> column (WKB) ' +
    'and the GeoParquet <code>geo</code> key in the file metadata.' +
    colList + ' Pick a row to inspect it.';
  $mapEmpty.classList.add('show');
}

function rowFor(idx) {
  return $tbody.querySelector(`tr[data-idx="${idx}"]`);
}

$tbody.addEventListener('click', e => {
  const tr = e.target.closest('tr[data-idx]');
  if (tr) select(+tr.dataset.idx, { from: 'table' });
});

$tbody.addEventListener('keydown', e => {
  const tr = e.target.closest('tr[data-idx]');
  if (!tr) return;
  if (e.key === 'Enter' || e.key === ' ') {
    e.preventDefault();
    select(+tr.dataset.idx, { from: 'table' });
  } else if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
    e.preventDefault();
    const next = e.key === 'ArrowDown' ? tr.nextElementSibling : tr.previousElementSibling;
    if (next) next.focus();
  }
});

// ─── Load ──────────────────────────────────────────────────────────────────
function syncPicks() {
  const value = $url.value.trim();
  $picks.querySelectorAll('.pick').forEach(item => {
    item.setAttribute('aria-pressed', String(item.dataset.url === value));
  });
}

function formatTime(ms) {
  return ms < 1000 ? `${ms.toFixed(0)} ms` : `${(ms / 1000).toFixed(2)} s`;
}

async function loadCozip() {
  const url = $url.value.trim();
  if (!url) return;
  const token = ++loadToken;
  currentUrl = url;
  $go.disabled = true;
  hidePanel();
  $mapEmpty.classList.remove('show');
  showError(null); showInfo(null);
  $loadText.textContent = 'Reading the index and the manifest…';
  $loading.classList.add('show');
  $statCount.textContent = $statTime.textContent = $statGeom.textContent = '-';
  clearTable('Loading…');

  const t0 = performance.now();
  try {
    const rows = await read(url);
    if (token !== loadToken) return;
    // 64-bit integers arrive as BigInt; byte offsets fit a Number up to 2^53
    for (const row of rows) {
      for (const k in row) {
        if (typeof row[k] === 'bigint') row[k] = Number(row[k]);
      }
    }
    const ms = performance.now() - t0;
    currentRows = rows;
    renderManifestTable(rows);
    $statCount.textContent = rows.length.toLocaleString();
    $statTime.textContent = formatTime(ms);

    $loadText.textContent = 'Drawing the map…';
    const ready = await mapReady;
    if (token !== loadToken) return;
    const geomKey = detectGeometryKey(rows);
    if (!geomKey) {
      setFeatures({ type: 'FeatureCollection', features: [] });
      showNoGeometry(rows);
      $statGeom.textContent = 'none';
      return;
    }

    const { fc, nFailed, typeCounts } = rowsToGeoJSON(rows, geomKey);
    setFeatures(fc);
    const typeSummary = Object.entries(typeCounts).map(([t, c]) => `${c} ${t}`).join(', ');
    $statGeom.textContent = typeSummary + (nFailed ? ` (${nFailed} failed)` : '');
    if (nFailed) showInfo(`${nFailed} of ${rows.length} geometries could not be parsed and are not on the map.`);
    const b = geoJSONBounds(fc);
    if (ready && b) map.fitBounds(b, { padding: 60, duration: 1200, maxZoom: 8 });
  } catch (e) {
    if (token !== loadToken) return;
    console.error(e);
    currentRows = null;
    setFeatures({ type: 'FeatureCollection', features: [] });
    clearTable('Nothing loaded.');
    showError(e.message || String(e));
  } finally {
    if (token === loadToken) {
      $loading.classList.remove('show');
      $go.disabled = false;
    }
  }
}

// ─── Selection ─────────────────────────────────────────────────────────────
function select(idx, { from } = {}) {
  if (!currentRows || !currentRows[idx]) return;
  if (map && currentSelection !== null && featureIds.has(currentSelection)) {
    map.setFeatureState({ source: 'cozip', id: currentSelection }, { selected: false });
  }
  $tbody.querySelectorAll('tr.selected').forEach(r => r.classList.remove('selected'));
  currentSelection = idx;
  const row = currentRows[idx];
  const tr = rowFor(idx);
  if (tr) tr.classList.add('selected');
  if (map && featureIds.has(idx)) {
    map.setFeatureState({ source: 'cozip', id: idx }, { selected: true });
    const geometry = geometries.get(idx);
    if (geometry) flyToGeometry(geometry);
  }
  $title.textContent = row.name ?? `entry ${idx}`;
  $dlFilename.textContent = row.name ?? `entry_${idx}`;
  $range.textContent = (typeof row.offset === 'number' && typeof row.size === 'number')
    ? `Range: bytes=${row.offset}-${row.offset + row.size - 1}`
    : '';
  renderMeta(row);
  $panel.classList.add('open');
  // after the panel opens, so the row lands in the table that is left
  if (tr && from === 'map') {
    const wrap = tr.closest('.play-table-wrap');
    const head = wrap.querySelector('thead').offsetHeight;
    if (tr.offsetTop - head < wrap.scrollTop || tr.offsetTop + tr.offsetHeight > wrap.scrollTop + wrap.clientHeight) {
      wrap.scrollTop = tr.offsetTop - head - (wrap.clientHeight - head) / 2;
    }
  }
}

function flyToGeometry(geometry) {
  const pts = [];
  eachPoint(geometry.coordinates, p => pts.push(p));
  if (!pts.length) return;
  let sx = 0, sy = 0;
  for (const [x, y] of pts) { sx += x; sy += y; }
  map.flyTo({ center: [sx / pts.length, sy / pts.length], zoom: Math.max(map.getZoom(), 4), duration: 800, essential: true });
}

function renderMeta(row) {
  const keys = Object.keys(row);
  const priority = ['name', 'offset', 'size'];
  const last = ['cozip:location'];
  const rest = keys.filter(k => !priority.includes(k) && !last.includes(k) && !GEOMETRY_KEYS.test(k)).sort();
  const ordered = [...priority.filter(k => k in row), ...rest, ...last.filter(k => k in row)];

  const html = ordered.map(k => {
    const v = row[k];
    let cls = 'meta-value', display;
    if (v === null || v === undefined) display = '<span class="null">null</span>';
    else if (v instanceof Uint8Array || ArrayBuffer.isView(v)) {
      cls = 'meta-value binary';
      display = `&lt;binary ${v.byteLength} bytes&gt;`;
    }
    else if (typeof v === 'object') display = esc(JSON.stringify(v));
    else if (typeof v === 'number' && k === 'size')
      display = `${esc(v.toLocaleString())} <span class="unit">B (${humanBytes(v)})</span>`;
    else display = esc(String(v));
    if (k === 'cozip:location') cls = 'meta-value vsi';
    return `<div class="meta-row"><div class="meta-key">${esc(k)}</div><div class="${cls}">${display}</div></div>`;
  }).join('');
  const geomKey = detectGeometryKey([row]);
  let geomNote = '';
  if (geomKey) {
    const g = parseGeometryValue(row[geomKey]);
    if (g) geomNote = `<div class="meta-row"><div class="meta-key">${esc(geomKey)}</div><div class="meta-value binary">${esc(g.type)} (parsed)</div></div>`;
  }
  $body.innerHTML = html + geomNote;
}

function humanBytes(n) {
  if (n < 1024) return n + ' B';
  if (n < 1024 ** 2) return (n / 1024).toFixed(1) + ' KiB';
  if (n < 1024 ** 3) return (n / 1024 ** 2).toFixed(2) + ' MiB';
  return (n / 1024 ** 3).toFixed(2) + ' GiB';
}

function esc(s) {
  return String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

function hidePanel() {
  if (map && currentSelection !== null && featureIds.has(currentSelection) && map.getSource('cozip')) {
    map.setFeatureState({ source: 'cozip', id: currentSelection }, { selected: false });
  }
  $tbody.querySelectorAll('tr.selected').forEach(r => r.classList.remove('selected'));
  currentSelection = null;
  $panel.classList.remove('open');
  $title.textContent = 'Pick a row or a feature';
  $range.textContent = '';
  $body.innerHTML = '';
}

function showError(msg) {
  if (!msg) { $error.classList.remove('show'); $error.textContent = ''; return; }
  $error.textContent = msg;
  $error.classList.add('show');
}

function showInfo(text) {
  if (!text) { $info.classList.remove('show'); $info.textContent = ''; return; }
  $info.textContent = text;
  $info.classList.add('show');
}

// ─── Copy and download ─────────────────────────────────────────────────────
async function copyTo(text, btn) {
  try {
    await navigator.clipboard.writeText(text);
    const orig = btn.textContent;
    btn.classList.add('copied'); btn.textContent = 'Copied';
    setTimeout(() => { btn.textContent = orig; btn.classList.remove('copied'); }, 1200);
  } catch (e) { showError('Clipboard: ' + e.message); }
}

$copyVsi.addEventListener('click', () => {
  if (currentSelection === null) return;
  const row = currentRows[currentSelection];
  if (row['cozip:location']) copyTo(row['cozip:location'], $copyVsi);
});

$copyName.addEventListener('click', () => {
  if (currentSelection === null) return;
  copyTo(currentRows[currentSelection].name || '', $copyName);
});

$download.addEventListener('click', async () => {
  if (currentSelection === null) return;
  const row = currentRows[currentSelection];
  if (typeof row.offset !== 'number' || typeof row.size !== 'number') {
    showError('This row has no offset and size to fetch.'); return;
  }
  $download.disabled = true;
  const label = $download.querySelector('.play-download-label');
  const orig = label.textContent;
  label.textContent = 'Fetching…';
  try {
    const res = await fetch(currentUrl, {
      headers: { Range: `bytes=${row.offset}-${row.offset + row.size - 1}` }
    });
    if (!res.ok && res.status !== 206) throw new Error(`HTTP ${res.status}`);
    const blob = await res.blob();
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = (row.name || `entry_${currentSelection}`).split('/').pop();
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
    label.textContent = 'Saved';
    setTimeout(() => { label.textContent = orig; }, 1400);
  } catch (e) {
    showError('Download failed: ' + e.message);
    label.textContent = orig;
  } finally {
    $download.disabled = false;
  }
});

// ─── Controls ──────────────────────────────────────────────────────────────
$form.addEventListener('submit', e => {
  e.preventDefault();
  loadCozip();
});

$picks.addEventListener('click', e => {
  const pick = e.target.closest('.pick');
  if (!pick) return;
  $url.value = pick.dataset.url;
  syncPicks();
  loadCozip();
});

$url.addEventListener('input', syncPicks);
$close.addEventListener('click', hidePanel);
document.addEventListener('keydown', e => {
  if (e.key === 'Escape' && currentSelection !== null) hidePanel();
});

// a ?url= parameter opens that archive instead of the first example
const asked = new URLSearchParams(window.location.search).get('url');
if (asked) $url.value = asked;
syncPicks();

startMap();
loadCozip();
