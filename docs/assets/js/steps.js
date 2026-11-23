// Step figures drawn inline, as on the landing page: the sentence of the step
// on top, the drawing, then the step names. The reader picks a step with its
// name, the arrows, a click on the drawing or the arrow keys; nothing moves on
// its own.
//   <figure class="steps" data-step="1">
//     <div class="steps-say"><p data-k="1" data-name="index">...</p>...</div>
//     <div class="steps-view"><svg>...</svg></div>
//   </figure>
// The figure's data-step selects what shows: data-k lists the steps an element
// appears in, data-dim the steps it fades in, data-on the steps it is lit in.
// The rules are in docs.css.

(function () {
  'use strict';

  var prevIcon = '<svg viewBox="0 0 16 16" aria-hidden="true"><path d="M10 3 5 8l5 5"/></svg>';
  var nextIcon = '<svg viewBox="0 0 16 16" aria-hidden="true"><path d="M6 3l5 5-5 5"/></svg>';

  function button(className, html, label) {
    var b = document.createElement('button');
    b.type = 'button';
    b.className = className;
    b.innerHTML = html;
    if (label) b.setAttribute('aria-label', label);
    return b;
  }

  document.querySelectorAll('figure.steps').forEach(function (figure) {
    var notes = Array.prototype.slice.call(figure.querySelectorAll('.steps-say [data-k]'));
    var view = figure.querySelector('.steps-view');
    var total = notes.length;
    if (!total || !view) return;

    var nav = document.createElement('nav');
    nav.className = 'steps-nav';
    nav.setAttribute('aria-label', 'Steps');
    var prev = button('steps-arrow', prevIcon, 'Previous step');
    var next = button('steps-arrow', nextIcon, 'Next step');
    var names = document.createElement('div');
    names.className = 'steps-names';
    var tabs = notes.map(function (note, i) {
      var name = note.getAttribute('data-name');
      var b = button('steps-name', name, 'Step ' + (i + 1) + ', ' + name);
      b.addEventListener('click', function () { go(i + 1); });
      names.appendChild(b);
      return b;
    });
    var said = document.createElement('span');
    said.className = 'steps-said';
    said.setAttribute('aria-live', 'polite');
    nav.appendChild(prev);
    nav.appendChild(names);
    nav.appendChild(next);
    nav.appendChild(said);
    figure.insertBefore(nav, figure.querySelector('figcaption'));

    var step = 0;
    function go(n) {
      var to = Math.min(Math.max(n, 1), total);
      if (to === step) return;
      step = to;
      figure.setAttribute('data-step', String(step));
      tabs.forEach(function (b, i) {
        b.classList.toggle('is-active', i + 1 === step);
        b.setAttribute('aria-pressed', i + 1 === step ? 'true' : 'false');
      });
      prev.disabled = step === 1;
      next.disabled = step === total;
      said.textContent = notes[step - 1].textContent;
    }

    prev.addEventListener('click', function () { go(step - 1); });
    next.addEventListener('click', function () { go(step + 1); });
    view.setAttribute('tabindex', '0');
    view.addEventListener('click', function () { go(step === total ? 1 : step + 1); });
    figure.addEventListener('keydown', function (event) {
      if (event.target.closest && event.target.closest('a')) return;
      if (event.key === 'ArrowRight') go(step + 1);
      else if (event.key === 'ArrowLeft') go(step - 1);
      else if (event.key === 'Home') go(1);
      else if (event.key === 'End') go(total);
      else return;
      event.preventDefault();
    });
    figure.classList.add('is-ready');
    go(1);
  });
})();
