(function () {
  'use strict';

  var deck = document.getElementById('deck');
  var bar = document.getElementById('progress');
  var slides = [];
  var cur = 0;
  var rendered = false;

  function render(i) {
    cur = Math.max(0, Math.min(i, slides.length - 1));

    slides.forEach(function (s, idx) {
      s.classList.toggle('active', idx === cur);
      s.setAttribute('aria-hidden', idx === cur ? 'false' : 'true');
      s.inert = idx !== cur;
    });

    if (bar) bar.style.width = ((cur + 1) / slides.length * 100) + '%';
  }

  function goTo(i) {
    var target = Math.max(0, Math.min(i, slides.length - 1));

    if (!rendered || target === cur || !document.startViewTransition) {
      render(target);
      rendered = true;
      return;
    }

    document.documentElement.setAttribute(
      'data-deck-direction',
      target > cur ? 'next' : 'prev'
    );

    var transition = document.startViewTransition(function () {
      render(target);
    });

    function cleanDirection() {
      document.documentElement.removeAttribute('data-deck-direction');
    }

    transition.finished.then(cleanDirection, cleanDirection);
    // a quick second key press skips this transition, which is expected
    transition.ready.catch(function () {});
  }

  // A slide with data-steps takes next and prev itself until its first or last step.
  function step(by) {
    var s = slides[cur];
    if (!s || !s.dataset.steps) return false;
    var at = Number(s.dataset.step || 1) + by;
    if (at < 1 || at > Number(s.dataset.steps)) return false;
    s.dataset.step = String(at);
    return true;
  }

  function next() {
    if (!step(1)) goTo(cur + 1);
  }

  function prev() {
    if (!step(-1)) goTo(cur - 1);
  }


  document.addEventListener('keydown', function (e) {
    if (e.target.closest && e.target.closest('input')) return;
    if (e.key === 'ArrowRight' || e.key === ' ') {
      e.preventDefault();
      next();
    }
    if (e.key === 'ArrowLeft') {
      e.preventDefault();
      prev();
    }
    if (e.key === 'Home') {
      e.preventDefault();
      goTo(0);
    }
    if (e.key === 'End') {
      e.preventDefault();
      goTo(slides.length - 1);
    }
  });

  var touchX = 0;

  if (deck) {
    deck.addEventListener('click', function (event) {
      if (event.target.closest('a, button, input, [data-interactive], .path-fig svg')) return;
      next();
    });

    deck.addEventListener('touchstart', function (e) {
      touchX = e.touches[0].clientX;
    }, { passive: true });

    deck.addEventListener('touchend', function (e) {
      var dx = e.changedTouches[0].clientX - touchX;
      if (Math.abs(dx) > 50) {
        dx < 0 ? next() : prev();
      }
    }, { passive: true });
  }

  function resize() {
    var s = Math.min(window.innerWidth / 960, window.innerHeight / 540);
    deck.style.transform = 'scale(' + s + ')';
  }

  window.addEventListener('resize', resize);
  window.addEventListener('orientationchange', resize);

  slides = Array.prototype.slice.call(deck.querySelectorAll('.slide'));

  var requested = Number(new URLSearchParams(window.location.search).get('slide'));
  goTo(Number.isInteger(requested) && requested > 0 ? requested - 1 : 0);
  resize();
})();
