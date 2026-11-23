/* The copy button of the install line, with the hearts from the Rumi and Karu sites. */
(function () {
  'use strict';

  var reduceMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  var copyTimer = 0;

  function fallbackCopy(text) {
    var input = document.createElement('textarea');
    input.value = text;
    input.setAttribute('readonly', '');
    input.style.position = 'fixed';
    input.style.opacity = '0';
    document.body.appendChild(input);
    input.select();
    var copied = false;
    try {
      copied = document.execCommand('copy');
    } catch (error) {
      copied = false;
    }
    input.remove();
    return copied;
  }

  function celebrate(command, heart) {
    command.classList.remove('is-copied');
    heart.classList.remove('is-loved');
    void heart.offsetWidth;
    command.classList.add('is-copied');
    heart.classList.add('is-loved');
    if (reduceMotion) return;
    var box = heart.getBoundingClientRect();
    var tints = ['#f05b7f', '#492ae8', '#f7cc09', '#c4ffc2'];
    for (var i = 0; i < 16; i += 1) {
      var angle = -Math.PI / 2 + (Math.random() - 0.5) * 2.6;
      var reach = 50 + Math.random() * 70;
      var spark = document.createElement('span');
      spark.className = 'heart-spark';
      spark.style.left = (box.left + box.width / 2) + 'px';
      spark.style.top = (box.top + box.height / 2) + 'px';
      spark.style.setProperty('--dx', Math.round(Math.cos(angle) * reach) + 'px');
      spark.style.setProperty('--dy', Math.round(Math.sin(angle) * reach) + 'px');
      spark.style.setProperty('--size', Math.round(8 + Math.random() * 10) + 'px');
      spark.style.setProperty('--spin', Math.round((Math.random() - 0.5) * 70) + 'deg');
      spark.style.setProperty('--tint', tints[i % tints.length]);
      spark.style.animationDelay = Math.round(Math.random() * 180) + 'ms';
      spark.addEventListener('animationend', function () { this.remove(); });
      document.body.appendChild(spark);
    }
  }

  var button = document.querySelector('.copy-command');
  var heart = document.querySelector('.heart');
  if (!button) return;
  var root = button.closest('[data-copy-root]');
  var command = button.closest('.install-command');

  command.addEventListener('click', function (event) {
    if (event.target.closest('.copy-command')) return;
    button.click();
  });

  button.addEventListener('click', function () {
    var text = button.getAttribute('data-copy') || '';
    var feedback = root.querySelector('.copy-feedback');
    var copy = navigator.clipboard && window.isSecureContext
      ? navigator.clipboard.writeText(text).then(function () { return true; }).catch(function () { return fallbackCopy(text); })
      : Promise.resolve(fallbackCopy(text));

    copy.then(function (copied) {
      window.clearTimeout(copyTimer);
      button.classList.toggle('is-copied', copied);
      root.classList.toggle('is-copied', copied);
      button.querySelector('span').textContent = copied ? 'Copied' : 'Copy';
      feedback.textContent = copied ? 'Install line copied.' : 'Copy failed. Select the line manually.';
      if (copied && heart) celebrate(command, heart);
      copyTimer = window.setTimeout(function () {
        button.classList.remove('is-copied');
        root.classList.remove('is-copied');
        command.classList.remove('is-copied');
        if (heart) heart.classList.remove('is-loved');
        button.querySelector('span').textContent = 'Copy';
      }, 1800);
    });
  });
})();
