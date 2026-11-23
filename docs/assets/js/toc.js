(function () {
  'use strict';
  var links = Array.prototype.slice.call(document.querySelectorAll('.toc-sections a[href^="#"]'));
  if (!links.length) return;

  var items = [];
  links.forEach(function (link) {
    var target = document.getElementById(decodeURIComponent(link.getAttribute('href').slice(1)));
    if (target) items.push({ link: link, target: target });
  });
  if (!items.length) return;

  var rail = links[0].closest('.toc');
  var current = null;

  function mark(item) {
    if (item === current) return;
    if (current) current.link.classList.remove('is-active');
    current = item;
    if (!item) return;
    item.link.classList.add('is-active');
    item.link.setAttribute('aria-current', 'location');
    if (rail && rail.scrollHeight > rail.clientHeight) {
      var top = item.link.getBoundingClientRect().top - rail.getBoundingClientRect().top + rail.scrollTop;
      if (top < rail.scrollTop || top > rail.scrollTop + rail.clientHeight - 40) {
        rail.scrollTop = top - rail.clientHeight / 3;
      }
    }
  }

  function update() {
    var line = window.innerHeight * 0.28;
    var found = null;
    for (var i = 0; i < items.length; i++) {
      if (items[i].target.getBoundingClientRect().top <= line) found = items[i];
      else break;
    }
    var bottom = window.innerHeight + window.scrollY >= document.documentElement.scrollHeight - 4;
    if (bottom) found = items[items.length - 1];
    if (current && current !== found) current.link.removeAttribute('aria-current');
    mark(found);
  }

  var queued = false;
  function onScroll() {
    if (queued) return;
    queued = true;
    window.requestAnimationFrame(function () {
      queued = false;
      update();
    });
  }

  window.addEventListener('scroll', onScroll, { passive: true });
  window.addEventListener('resize', onScroll);
  update();
})();
