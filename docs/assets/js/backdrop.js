// Lights runs of cells in the grid that .ambient already draws, as ranges of
// bytes arriving: each run starts somewhere on a row and fills left to right,
// in the four colours of the cozip palette. Same 64px pitch as the CSS background.

(function (cozip) {
  'use strict';

  var TONE = ['#492ae8', '#f05b7f', '#f7cc09', '#91f2bc'];

  var CELL = 64;
  var EVERY = 520;  // ms between two runs
  var STEP = 70;    // ms between two cells of a run
  var TAIL = 2400;  // ms for a lit cell to go dark
  var PEAK = 0.12;  // alpha of a cell the instant it lights

  cozip.backdrop = function (canvas) {
    if (matchMedia('(prefers-reduced-motion: reduce)').matches) return;

    var ctx = canvas.getContext('2d');
    var cols, rows, life, tone, runs = [], owed = EVERY, last = 0, count = 0;

    function measure() {
      var w = window.innerWidth;
      var h = window.innerHeight;
      var dpr = Math.min(window.devicePixelRatio || 1, 2);

      canvas.width = w * dpr;
      canvas.height = h * dpr;
      canvas.style.width = w + 'px';
      canvas.style.height = h + 'px';
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

      cols = Math.ceil(w / CELL) + 1;
      rows = Math.ceil(h / CELL) + 1;
      life = new Float32Array(cols * rows);
      tone = new Uint8Array(cols * rows);
      runs = [];
    }

    function paint(now) {
      // a backgrounded tab hands back a huge delta, so cap it
      var dt = Math.min(now - last, 100);
      last = now;

      ctx.clearRect(0, 0, window.innerWidth, window.innerHeight);

      owed += dt;
      if (owed > EVERY) {
        owed = 0;
        var length = 2 + Math.floor(Math.random() * 5);
        runs.push({
          at: Math.floor(Math.random() * rows) * cols + Math.floor(Math.random() * Math.max(cols - length, 1)),
          left: length,
          wait: 0,
          tone: count++ % TONE.length
        });
      }

      runs = runs.filter(function (run) {
        run.wait -= dt;
        while (run.left > 0 && run.wait <= 0) {
          life[run.at] = 1;
          tone[run.at] = run.tone;
          run.at += 1;
          run.left -= 1;
          run.wait += STEP;
        }
        return run.left > 0;
      });

      for (var i = 0; i < life.length; i++) {
        if (life[i] <= 0) continue;

        life[i] -= dt / TAIL;
        if (life[i] <= 0) {
          life[i] = 0;
          continue;
        }

        ctx.globalAlpha = life[i] * life[i] * PEAK;
        ctx.fillStyle = TONE[tone[i]];
        ctx.fillRect(
          (i % cols) * CELL + 1,
          Math.floor(i / cols) * CELL + 1,
          CELL - 2,
          CELL - 2
        );
      }

      ctx.globalAlpha = 1;
      requestAnimationFrame(paint);
    }

    window.addEventListener('resize', measure);
    measure();

    requestAnimationFrame(function (t) {
      last = t;
      paint(t);
    });
  };

  var canvas = document.getElementById('backdrop');
  if (canvas) cozip.backdrop(canvas);
})((window.cozip = window.cozip || {}));
