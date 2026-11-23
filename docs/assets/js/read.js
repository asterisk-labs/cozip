// The landing figure: a cozip drawn as a column of its parts, from the index
// at byte 0 down to the ZIP directory. Four steps, chosen by the reader with
// the arrows, the step names or the arrow keys, as on the GeoZL page:
//   index     what the index at byte 0 holds: the offset and size of the manifest
//   manifest  the manifest opened as a table, each row beside its file
//   filter    the table filtered like any other
//   read      only the picked files read, one range request each
// Each step lights its part and dims the rest. cozip.step(n) shows step n.

(function (cozip) {
  'use strict';

  var NS = 'http://www.w3.org/2000/svg';
  var W = 660;
  var H = 430;

  var VIOLET = '#492ae8';
  var MANIFEST = VIOLET;   // as strong as the index: the two parts cozip adds
  // the files take muted tints, so the index and the manifest stand out
  var TONES = ['#f2d4dc', '#f3e6b3', '#d8efd6', '#d3e5f3', '#f3dccd', '#e4e2d6'];

  var READ = 'manifest = <span class="rd-fn">cozip.read</span>(<span class="rd-str">"s3://bucket/tiles.zip"</span>)';
  var STEPS = [
    { name: 'Index', text: 'The index at byte 0 gives the +offset and size of the manifest+.', code: READ },
    { name: 'Manifest', text: 'The manifest *is a table*, with one row per file and the offset and size of each.', code: READ },
    { name: 'Filter', text: 'Filter it +like any other table+, on any column you stored.',
      code: 'clear = manifest[manifest.cloud_cover &lt; 25]' },
    { name: 'Read', text: 'Read *only the files you picked*, one range request each.',
      code: '<span class="rd-fn">rasterio.open</span>(row[<span class="rd-str">"cozip:location"</span>])' }
  ];

  // the first six files of the reference archive the API page measures: a
  // million files of 1,000 bytes, 000000.tif to 999999.tif, with six metadata
  // columns. Offsets follow spec 5.1.3: the index starts at byte 51, a Flat
  // index is 41 bytes, and every payload follows a 30-byte local header and
  // its name. The manifest of the million rows starts at byte 1,040,000,134
  // and is 11,742,751 bytes long.
  var FILES = [
    ['000000.tif', 22.6, 1000], ['000001.tif', 19.9, 1000], ['000002.tif', 36.3, 1000],
    ['000003.tif', 17.9, 1000], ['000004.tif', 34.6, 1000], ['000005.tif', 94.8, 1000]
  ];
  var PICKED = [0, 1, 3];   // cloud_cover under 25
  var MANIFEST_SIZE = 11742751;
  var MANIFEST_OFFSET = 1040000134;
  (function () {
    var lfh = 51 + 41;
    FILES.forEach(function (f) {
      f.offset = lfh + 30 + f[0].length;
      lfh = f.offset + f[2];
    });
  })();

  // the archive column
  var AX = 20;
  var AY = 34;
  var AW = 152;
  var AH = 384;
  var BX = 42;
  var BW = 122;
  var INDEX = { y: 46, h: 30 };
  var FILE_Y = 88;
  var FILE_H = 34;
  var FILE_STEP = 42;
  var MAN = { y: 342, h: 34 };
  var DIR = { y: 384, h: 26 };

  // the panel on the right: the index fields, or the table
  var TX = 196;
  var TW = 450;
  var TH = FILE_Y + 6 * FILE_STEP - INDEX.y;
  var COL = { chip: 206, name: 224, cloud: 344, offset: 410, size: 478, location: 496 };
  var URI = 's3://bucket/tiles.zip';
  var LOCATION_CHARS = 20;
  var FW = 330;  // the index panel is narrower than the table

  function el(tag, attrs, parent) {
    var node = document.createElementNS(NS, tag);
    for (var key in attrs) node.setAttribute(key, attrs[key]);
    if (parent) parent.appendChild(node);
    return node;
  }

  function text(parent, x, y, value, cls, anchor) {
    var node = el('text', { x: x, y: y, 'class': cls }, parent);
    if (anchor) node.setAttribute('text-anchor', anchor);
    node.textContent = value;
    return node;
  }

  function group(n) {
    return String(n).replace(/\B(?=(\d{3})+(?!\d))/g, ',');
  }

  function fileY(i) {
    return FILE_Y + i * FILE_STEP;
  }

  function rowY(i) {
    return fileY(i) + FILE_H / 2;
  }

  function build(svg) {
    var v = {};

    var defs = el('defs', {}, svg);
    var hatch = el('pattern', { id: 'rd-hatch', width: 6, height: 6, patternUnits: 'userSpaceOnUse', patternTransform: 'rotate(45)' }, defs);
    el('rect', { width: 6, height: 6, fill: '#f4f4f6' }, hatch);
    el('line', { x1: 0, y1: 0, x2: 0, y2: 6, stroke: '#c9cbd3', 'stroke-width': 2 }, hatch);
    [['rd-head', VIOLET], ['rd-head-ink', '#000']].forEach(function (m) {
      var head = el('marker', { id: m[0], markerWidth: 8, markerHeight: 8, refX: 6.5, refY: 4, orient: 'auto' }, defs);
      el('path', { d: 'M0,0 L8,4 L0,8 Z', fill: m[1] }, head);
    });

    // the archive: a ZIP with its teeth down the left edge
    text(svg, AX, AY - 12, 'tiles.zip, 1,000,000 files', 'rd-title');
    el('rect', { 'class': 'rd-zip', x: AX, y: AY, width: AW, height: AH }, svg);
    for (var k = 0; AY + 5 + k * 6 < AY + AH - 8; k++) {
      el('rect', { 'class': 'rd-tooth', x: AX + (k % 2 ? 9 : 4), y: AY + 5 + k * 6, width: 6, height: 3.6 }, svg);
    }

    function block(y, h, cls, tone, label, sub) {
      var g = el('g', { 'class': 'rd-block ' + cls, style: '--tone:' + tone }, svg);
      el('rect', { 'class': 'rd-block-box', x: BX, y: y, width: BW, height: h }, g);
      text(g, BX + 10, y + h / 2 + 4.5, label, 'rd-block-label');
      if (sub) text(g, BX + BW - 14, y + h / 2 + 4, sub, 'rd-block-sub', 'end');
      return g;
    }

    v.index = block(INDEX.y, INDEX.h, 'is-index', VIOLET, 'index', 'byte 0');
    v.files = FILES.map(function (f, i) {
      var g = block(fileY(i), FILE_H, 'is-file', TONES[i], f[0], (f[2] / 1000) + ' kB');
      var x = BX + BW;
      var y = fileY(i);
      el('path', { 'class': 'rd-fold', d: 'M' + (x - 9) + ' ' + y + ' V' + (y + 9) + ' H' + x }, g);
      return g;
    });
    v.manifest = block(MAN.y, MAN.h, 'is-manifest', MANIFEST, 'manifest', '');
    var gx = BX + BW - 36;
    var gy = MAN.y + 8;
    var glyph = el('g', { 'class': 'rd-glyph' }, v.manifest);
    el('rect', { x: gx, y: gy, width: 26, height: 18 }, glyph);
    el('path', { d: 'M' + gx + ' ' + (gy + 6) + ' H' + (gx + 26) + ' M' + gx + ' ' + (gy + 12) + ' H' + (gx + 26) + ' M' + (gx + 10) + ' ' + gy + ' V' + (gy + 18) }, glyph);
    v.dir = block(DIR.y, DIR.h, 'is-dir', '#fff', 'ZIP directory', '');

    // index: the arrow to the manifest, and the fields of the index
    var x0 = BX + BW;
    var y0 = INDEX.y + INDEX.h / 2;
    var y1 = MAN.y + MAN.h / 2;
    var panelBottom = INDEX.y + 236;
    v.pointer = el('path', {
      'class': 'rd-pointer',
      d: 'M' + (TX + 70) + ' ' + panelBottom + ' L' + (x0 + 8) + ' ' + (y1 - 2),
      'marker-end': 'url(#rd-head)'
    }, svg);

    v.fields = el('g', { 'class': 'rd-panel' }, svg);
    el('rect', { 'class': 'rd-panel-box', x: TX, y: INDEX.y, width: FW, height: 236 }, v.fields);
    el('path', { 'class': 'rd-panel-tie', d: 'M' + (BX + BW) + ' ' + y0 + ' H' + TX }, v.fields);
    text(v.fields, TX + 14, INDEX.y + 22, 'the index, 41 bytes from byte 51', 'rd-col');
    var FIELDS = [
      ['magic', 'CZIP', ''],
      ['version', '1', ''],
      ['profile', '1', 'Flat: one priority file'],
      ['entries', '1', ''],
      ['name', '__metadata__', 'the manifest'],
      ['offset', group(MANIFEST_OFFSET), 'where it starts'],
      ['size', group(MANIFEST_SIZE), 'how long it is']
    ];
    FIELDS.forEach(function (f, i) {
      var y = INDEX.y + 50 + i * 27;
      if (i) el('path', { 'class': 'rd-panel-rule', d: 'M' + (TX + 14) + ' ' + (y - 17) + ' H' + (TX + FW - 14) }, v.fields);
      text(v.fields, TX + 14, y, f[0], 'rd-field-key');
      text(v.fields, TX + 92, y, f[1], 'rd-field-value' + (i >= 4 ? ' is-key' : ''));
      if (f[2]) text(v.fields, TX + FW - 14, y, f[2], 'rd-field-note', 'end');
    });

    // manifest: the table, each row beside its file
    v.table = el('g', { 'class': 'rd-panel' }, svg);
    el('rect', { 'class': 'rd-panel-box', x: TX, y: INDEX.y, width: TW, height: TH }, v.table);
    text(v.table, COL.name, INDEX.y + 19, 'name', 'rd-col');
    text(v.table, COL.cloud, INDEX.y + 19, 'cloud_cover', 'rd-col', 'end');
    text(v.table, COL.offset, INDEX.y + 19, 'offset', 'rd-col', 'end');
    text(v.table, COL.size, INDEX.y + 19, 'size', 'rd-col', 'end');
    text(v.table, COL.location, INDEX.y + 19, 'cozip:location', 'rd-col');
    v.manifestTie = el('path', { 'class': 'rd-panel-tie', d: 'M' + (BX + BW) + ' ' + y1 + ' H' + (TX + 30) + ' V' + (INDEX.y + TH) }, v.table);
    v.rows = FILES.map(function (f, i) {
      var y = rowY(i);
      var g = el('g', { 'class': 'rd-row', style: '--tone:' + TONES[i] }, v.table);
      el('rect', { 'class': 'rd-pick', x: TX + 2, y: y - 15, width: TW - 4, height: 30 }, g);
      el('rect', { 'class': 'rd-chip', x: COL.chip, y: y - 5.5, width: 10, height: 10 }, g);
      text(g, COL.name, y + 4, f[0], 'rd-cell');
      text(g, COL.cloud, y + 4, f[1].toFixed(1), 'rd-cell', 'end');
      text(g, COL.offset, y + 4, group(f.offset), 'rd-cell', 'end');
      text(g, COL.size, y + 4, group(f[2]), 'rd-cell', 'end');
      // the reader adds the location, a GDAL path; it is never stored
      var location = '/vsisubfile/' + f.offset + '_' + f[2] + ',/vsis3/' + URI.slice(5);
      text(g, COL.location, y + 4, location.slice(0, LOCATION_CHARS) + '\u2026', 'rd-cell rd-location');
      var link = el('path', { 'class': 'rd-link', d: 'M' + (BX + BW) + ' ' + y + ' H' + TX }, svg);
      var read = el('path', { 'class': 'rd-read', d: 'M' + (BX + BW + 2) + ' ' + y + ' H' + (TX - 4), 'marker-end': 'url(#rd-head-ink)' }, svg);
      return { g: g, link: link, read: read };
    });
    v.checks = PICKED.map(function (k) {
      return el('path', { 'class': 'rd-check', d: 'M' + (TX + TW + 3) + ' ' + rowY(k) + ' l3.5 3.5 l6 -7' }, svg);
    });
    // "never read" takes the red marker; the band is placed once the text has a width
    v.dirNote = el('g', { 'class': 'rd-note' }, svg);
    v.dirRed = el('rect', { 'class': 'rd-red' }, v.dirNote);
    v.dirLead = text(v.dirNote, TX + 4, DIR.y + DIR.h / 2 + 4.5, 'The ZIP directory is', '');
    v.dirKey = text(v.dirNote, TX + 4, DIR.y + DIR.h / 2 + 4.5, 'never read.', '');
    return v;
  }

  function set(node, cls, on) {
    if (node.classList.contains(cls) !== on) node.classList.toggle(cls, on);
  }

  function show(v, s) {
    var picked = function (i) { return PICKED.indexOf(i) >= 0; };

    // the index and the manifest never fade: they are what cozip adds to the ZIP
    set(v.index, 'is-lit', s === 0);
    set(v.manifest, 'is-lit', s === 1);
    set(v.dir, 'is-dim', s !== 3);
    set(v.dir, 'is-noted', s === 3);
    set(v.dirNote, 'is-shown', s === 3);
    if (s === 3) {
      v.dirKey.setAttribute('x', (TX + 4 + v.dirLead.getComputedTextLength() + 4).toFixed(1));
      var b = v.dirKey.getBBox();
      v.dirRed.setAttribute('x', (b.x - 1).toFixed(1));
      v.dirRed.setAttribute('y', (b.y + b.height * 0.4).toFixed(1));
      v.dirRed.setAttribute('width', (b.width - 2).toFixed(1));
      v.dirRed.setAttribute('height', (b.height * 0.55).toFixed(1));
    }
    set(v.pointer, 'is-shown', s === 0);
    set(v.fields, 'is-shown', s === 0);
    set(v.table, 'is-shown', s >= 1);
    set(v.manifestTie, 'is-dim', s >= 2);

    v.files.forEach(function (g, i) {
      set(g, 'is-dim', s === 0 || (s >= 2 && !picked(i)));
      set(g, 'is-hit', s === 3 && picked(i));
    });
    v.rows.forEach(function (r, i) {
      set(r.g, 'is-picked', s >= 2 && picked(i));
      set(r.g, 'is-dim', s >= 2 && !picked(i));
      set(r.link, 'is-shown', s === 1 || s === 2);
      set(r.link, 'is-dim', s === 2 && !picked(i));
      set(r.read, 'is-shown', s === 3 && picked(i));
    });
    v.checks.forEach(function (c) { set(c, 'is-shown', s === 3); });
  }

  cozip.readFigure = function (figure) {
    var head = document.createElement('div');
    head.className = 'read-step';
    head.innerHTML = '<span class="read-n"></span><p class="read-text"></p>';
    figure.appendChild(head);
    var code = document.createElement('code');
    code.className = 'read-code';
    figure.appendChild(code);
    var svg = el('svg', { viewBox: '0 0 ' + W + ' ' + H, role: 'img', 'aria-label': figure.getAttribute('data-label') || '' }, figure);

    // the carousel of the GeoZL page: previous, the four steps, next
    var nav = document.createElement('div');
    nav.className = 'read-carousel';
    nav.setAttribute('role', 'group');
    nav.setAttribute('aria-label', 'Steps of a read');
    var arrow = function (d, label) {
      var b = document.createElement('button');
      b.type = 'button';
      b.className = 'read-carousel__button';
      b.setAttribute('aria-label', label);
      b.innerHTML = '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="' + d + '"></path></svg>';
      return b;
    };
    var prev = arrow('m15 18-6-6 6-6', 'Previous step');
    var next = arrow('m9 18 6-6-6-6', 'Next step');
    var steps = document.createElement('div');
    steps.className = 'read-carousel__steps';
    var dots = STEPS.map(function (step, i) {
      var b = document.createElement('button');
      b.type = 'button';
      b.className = 'read-carousel__dot';
      b.textContent = step.name;
      b.setAttribute('aria-label', 'Step ' + (i + 1) + ', ' + step.name);
      b.addEventListener('click', function () { go(i); });
      steps.appendChild(b);
      return b;
    });
    var said = document.createElement('span');
    said.className = 'read-carousel__announcement';
    said.setAttribute('aria-live', 'polite');
    nav.appendChild(prev);
    nav.appendChild(steps);
    nav.appendChild(next);
    nav.appendChild(said);
    figure.appendChild(nav);

    var view = build(svg);
    var current = -1;

    function go(i) {
      i = (i + STEPS.length) % STEPS.length;
      if (i === current) return;
      current = i;
      head.querySelector('.read-n').textContent = String(i + 1);
      // the key phrase of a step: *words* take the yellow marker, +words+ the green one
      head.querySelector('.read-text').innerHTML = STEPS[i].text
        .replace(/\*(.+?)\*/g, '<mark class="read-mark">$1</mark>')
        .replace(/\+(.+?)\+/g, '<mark class="read-mark is-green">$1</mark>');
      code.innerHTML = STEPS[i].code;
      dots.forEach(function (d, j) {
        d.classList.toggle('is-active', j === i);
        if (j === i) d.setAttribute('aria-current', 'step');
        else d.removeAttribute('aria-current');
      });
      said.textContent = 'Step ' + (i + 1) + ' of ' + STEPS.length + ': ' + STEPS[i].text.replace(/[*+]/g, '');
      show(view, i);
    }

    prev.addEventListener('click', function () { go(current - 1); });
    next.addEventListener('click', function () { go(current + 1); });
    document.addEventListener('keydown', function (event) {
      if (event.altKey || event.ctrlKey || event.metaKey || event.shiftKey) return;
      if (event.target.closest && event.target.closest('input, textarea, select, [contenteditable]')) return;
      if (event.key === 'ArrowLeft') go(current - 1);
      else if (event.key === 'ArrowRight') go(current + 1);
    });

    cozip.step = function (n) { go(n); };
    go(0);
  };

  var figure = document.getElementById('read');
  if (figure) cozip.readFigure(figure);
})((window.cozip = window.cozip || {}));
