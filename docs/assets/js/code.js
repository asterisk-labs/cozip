/* Syntax highlighting and copy buttons for code blocks. */
(function () {
  'use strict';

  function words(list) {
    var set = Object.create(null);
    list.split(/\s+/).forEach(function (word) { if (word) set[word] = true; });
    return set;
  }

  var C_STRING = /"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])+'/y;
  var C_COMMENT = /\/\/[^\n]*|\/\*[\s\S]*?\*\//y;
  var NUMBER = /(?:0[xX][\da-fA-F_]+|0[bB][01_]+|(?:\d[\d_]*(?:\.\d[\d_]*)?|\.\d[\d_]*)(?:[eE][+-]?\d+)?)[uUlLfFjJi]*/y;
  var IDENT = /[A-Za-z_]\w*/y;

  var C_TYPES = 'void char short int long float double signed unsigned bool _Bool ' +
    'size_t ptrdiff_t uintptr_t intptr_t';

  var c = {
    comment: C_COMMENT,
    string: C_STRING,
    keywords: words('if else for while do return switch case default break continue goto ' +
      'sizeof typedef struct enum union static inline const volatile extern register restrict'),
    literals: words('NULL true false'),
    types: words(C_TYPES),
    constants: 3,
    declarations: true,
    preprocessor: true
  };

  var LANGUAGES = {
    c: c,

    cpp: {
      comment: C_COMMENT,
      string: C_STRING,
      keywords: words('if else for while do return switch case default break continue ' +
        'sizeof typedef struct enum union static inline const volatile extern auto namespace ' +
        'class template typename using public private protected virtual override final ' +
        'constexpr new delete this try catch throw noexcept co_await co_return'),
      literals: words('nullptr NULL true false'),
      types: words(C_TYPES + ' std string string_view span vector optional expected ' +
        'uint8_t uint32_t uint64_t int64_t byte'),
      constants: 3,
      declarations: true,
      preprocessor: true
    },

    /* C-like pseudocode. */
    pseudo: {
      comment: C_COMMENT,
      string: C_STRING,
      keywords: words('if else for while do return sizeof break continue'),
      literals: words('NULL true false'),
      types: words(''),
      constants: 3
    },

    slang: {
      comment: C_COMMENT,
      string: C_STRING,
      keywords: c.keywords,
      literals: words('true false'),
      types: words(C_TYPES + ' uint uint2 uint3 uint4 int2 int3 int4 float2 float3 float4 half'),
      constants: 3,
      declarations: true
    },

    python: {
      comment: /#[^\n]*/y,
      string: /[rRbBfFuU]{0,2}(?:"""[\s\S]*?"""|'''[\s\S]*?'''|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*')/y,
      decorator: /@[A-Za-z_][\w.]*/y,
      keywords: words('and as assert async await break class continue def del elif else except ' +
        'finally for from global if import in is lambda nonlocal not or pass raise return try ' +
        'while with yield'),
      literals: words('True False None'),
      types: words('int float complex str bytes bytearray bool tuple list dict set frozenset ' +
        'object ndarray memoryview'),
      constants: 2,
      classes: true
    },

    r: {
      comment: /#[^\n]*/y,
      string: /"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'/y,
      ident: /(?:[A-Za-z]|\.(?!\d))[\w.]*/y,
      keywords: words('function if else for while repeat in next break return'),
      literals: words('TRUE FALSE NULL NA NaN Inf NA_integer_ NA_real_ NA_character_'),
      types: words('')
    },

    julia: {
      comment: /#=[\s\S]*?=#|#[^\n]*/y,
      string: /"""[\s\S]*?"""|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\\n])'/y,
      decorator: /@[A-Za-z_]\w*/y,
      ident: /[A-Za-z_][\w!]*/y,
      keywords: words('function end if elseif else for while in using import export module ' +
        'baremodule struct mutable return begin let do try catch finally quote macro const ' +
        'local global where isa abstract primitive break continue'),
      literals: words('true false nothing missing Inf NaN'),
      types: words(''),
      classes: true
    },

    ts: {
      comment: C_COMMENT,
      string: /"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'|`(?:\\.|[^`\\])*`/y,
      keywords: words('import from export default const let var function return new await async ' +
        'if else for of in while do switch case break continue class interface type extends ' +
        'implements as typeof instanceof void yield try catch finally throw delete enum declare ' +
        'readonly keyof'),
      literals: words('true false null undefined NaN Infinity'),
      types: words('number string boolean any unknown never object bigint symbol'),
      constants: 2,
      classes: true
    },

    sql: {
      comment: /--[^\n]*/y,
      string: /'(?:''|[^'])*'/y,
      keywords: words('SELECT FROM WHERE AND OR NOT AS IN IS LIKE ORDER BY GROUP LIMIT ' +
        'JOIN ON USING WITH INSTALL LOAD CREATE TABLE VIEW DISTINCT HAVING UNION ALL CASE ' +
        'WHEN THEN ELSE END'),
      literals: words('NULL TRUE FALSE'),
      types: words('')
    }
  };
  LANGUAGES.js = LANGUAGES.ts;

  var SH_COMMENT = /#[^\n]*/y;
  var SH_STRING = /"(?:\\.|[^"\\])*"|'[^']*'/y;
  var SH_VARIABLE = /\$\{?\w+\}?/y;
  var SH_WORD = /[^\s"'#|;&<>()$]+/y;

  function escape(text) {
    return text.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }

  function at(pattern, text, i) {
    pattern.lastIndex = i;
    var match = pattern.exec(text);
    return match && match[0].length ? match[0] : null;
  }

  function Output() {
    var html = '';
    var plain = '';
    this.text = function (s) { plain += s; };
    this.token = function (kind, s) {
      if (!kind) { plain += s; return; }
      html += escape(plain) + '<span class="tok-' + kind + '">' + escape(s) + '</span>';
      plain = '';
    };
    this.html = function () { return html + escape(plain); };
  }

  function lineStart(text, i) {
    var j = i - 1;
    while (j >= 0 && (text[j] === ' ' || text[j] === '\t')) j--;
    return j < 0 || text[j] === '\n';
  }

  /* Classify an identifier from its local context. */
  function classify(lang, word, after, state) {
    if (lang.keywords[word]) return 'kw';
    if (lang.literals[word]) return 'num';
    if (lang.types[word]) return 'type';
    if (state.previous === 'def' || state.previous === 'function') return 'fn';
    if (state.previous === 'class' || state.previous === 'struct' ||
        state.previous === 'enum' || state.previous === 'union') return 'type';
    if (lang.constants && word.length >= lang.constants && /^[A-Z][A-Z0-9_]*$/.test(word)) return 'num';
    var capital = /^[A-Z]/.test(word);
    var call = after[0] === '(';
    if (lang.classes && capital) return 'type';
    if (call) return 'fn';
    if (lang.declarations) {
      if (/_t$/.test(word) || (capital && /[a-z]/.test(word))) return 'type';
      if (state.typedef && state.depth === 0 && /^\s*;/.test(after)) return 'type';
      if (/^(?:\s+[A-Za-z_]\w*\s*[,;)=[(]|\s+\*+[A-Za-z_]\w*\s*[,;)=[])/.test(after)) return 'type';
    }
    return null;
  }

  function highlight(lang, text) {
    var out = new Output();
    var state = { previous: null, typedef: false, depth: 0 };
    var ident = lang.ident || IDENT;
    var i = 0;
    var m;
    while (i < text.length) {
      var ch = text[i];
      if ((m = at(lang.comment, text, i))) { out.token('com', m); i += m.length; continue; }
      if ((m = at(lang.string, text, i))) { out.token('str', m); i += m.length; continue; }
      if (lang.preprocessor && ch === '#' && lineStart(text, i) && (m = at(/#\s*[a-z]+/y, text, i))) {
        out.token('kw', m); i += m.length; continue;
      }
      if (lang.decorator && ch === '@' && (m = at(lang.decorator, text, i))) {
        out.token('kw', m); i += m.length; continue;
      }
      if (/[\d.]/.test(ch) && !/[\w.]/.test(text[i - 1] || '') && (m = at(NUMBER, text, i)) && /\d/.test(m)) {
        out.token('num', m); i += m.length; continue;
      }
      if ((m = at(ident, text, i))) {
        var kind = classify(lang, m, text.slice(i + m.length, i + m.length + 80), state);
        out.token(kind, m);
        if (m === 'typedef') { state.typedef = true; state.depth = 0; }
        state.previous = m;
        i += m.length;
        continue;
      }
      if (ch === '{') state.depth++;
      else if (ch === '}') state.depth--;
      else if (ch === ';' && state.depth === 0) state.typedef = false;
      if (!/\s/.test(ch)) state.previous = ch;
      out.text(ch);
      i++;
    }
    return out.html();
  }

  /* Highlight the first word of each shell command. */
  function shell(text) {
    var out = new Output();
    var command = true;
    var i = 0;
    var m;
    while (i < text.length) {
      var ch = text[i];
      if (ch === '#' && (i === 0 || /\s/.test(text[i - 1])) && (m = at(SH_COMMENT, text, i))) {
        out.token('com', m); i += m.length; continue;
      }
      if ((m = at(SH_STRING, text, i))) { out.token('str', m); i += m.length; command = false; continue; }
      if ((m = at(SH_VARIABLE, text, i))) { out.token('num', m); i += m.length; continue; }
      if ((m = at(SH_WORD, text, i))) {
        out.token(command ? 'fn' : m[0] === '-' ? 'type' : null, m);
        command = false;
        i += m.length;
        continue;
      }
      if (ch === '\n' || ch === '|' || ch === ';' || ch === '&') command = true;
      out.text(ch);
      i++;
    }
    return out.html();
  }

  Array.prototype.forEach.call(document.querySelectorAll('pre code[class*="language-"]'), function (code) {
    var name = /\blanguage-(\w+)/.exec(code.className)[1];
    var text = code.textContent;
    if (name === 'sh') code.innerHTML = shell(text);
    else if (LANGUAGES[name]) code.innerHTML = highlight(LANGUAGES[name], text);
  });

  /* Add a copy button to each block. */

  var ICONS = '<span class="code-copy-icon">' +
    '<svg class="icon-copy" viewBox="0 0 24 24" aria-hidden="true"><path d="M8 8h10v10H8z"></path><path d="M6 15H4V4h11v2"></path></svg>' +
    '<svg class="icon-check" viewBox="0 0 24 24" aria-hidden="true"><path d="m5 12.5 4.5 4.5L19 7.5"></path></svg>' +
    '</span>';

  function copyFallback(text) {
    var area = document.createElement('textarea');
    area.value = text;
    area.setAttribute('readonly', '');
    area.style.position = 'fixed';
    area.style.opacity = '0';
    document.body.appendChild(area);
    area.select();
    var ok = false;
    try { ok = document.execCommand('copy'); } catch (error) { ok = false; }
    area.remove();
    return ok;
  }

  function copy(text) {
    if (navigator.clipboard && window.isSecureContext) {
      return navigator.clipboard.writeText(text)
        .then(function () { return true; })
        .catch(function () { return copyFallback(text); });
    }
    return Promise.resolve(copyFallback(text));
  }

  var blocks = document.querySelectorAll('pre > code');
  if (!blocks.length) return;

  var feedback = document.createElement('p');
  feedback.className = 'code-feedback';
  feedback.setAttribute('aria-live', 'polite');
  document.body.appendChild(feedback);

  Array.prototype.forEach.call(blocks, function (code) {
    var pre = code.parentNode;
    var block = document.createElement('div');
    block.className = 'code-block';
    pre.parentNode.insertBefore(block, pre);
    block.appendChild(pre);

    var button = document.createElement('button');
    button.className = 'code-copy';
    button.type = 'button';
    button.setAttribute('aria-label', 'Copy code');
    button.innerHTML = ICONS;
    var toast = document.createElement('span');
    toast.className = 'code-copied';
    toast.setAttribute('aria-hidden', 'true');
    toast.textContent = 'Copied';
    block.appendChild(button);
    block.appendChild(toast);

    var timer = 0;
    button.addEventListener('click', function () {
      copy(code.textContent).then(function (ok) {
        block.classList.toggle('is-copied', ok);
        feedback.textContent = ok ? 'Code copied.' : 'Select the code and copy it by hand.';
        window.clearTimeout(timer);
        timer = window.setTimeout(function () {
          block.classList.remove('is-copied');
          feedback.textContent = '';
        }, 1600);
      });
    });
  });

  /* Show one example in several languages, a tab for each, and keep the
     chosen language on every example of the site. */

  var LANG_KEY = 'cozip-code-lang';
  var groups = [];

  function remembered() {
    try { return window.localStorage.getItem(LANG_KEY); } catch (error) { return null; }
  }

  function remember(lang) {
    try { window.localStorage.setItem(LANG_KEY, lang); } catch (error) { /* storage is off */ }
  }

  function choose(lang) {
    groups.forEach(function (group) { group.select(lang); });
  }

  Array.prototype.forEach.call(document.querySelectorAll('.code-tabs'), function (host, g) {
    var panels = Array.prototype.filter.call(host.children, function (child) {
      return child.classList.contains('code-block');
    });
    var bar = document.createElement('div');
    bar.className = 'code-tabs-bar';
    bar.setAttribute('role', 'tablist');
    bar.setAttribute('aria-label', 'Language');
    host.insertBefore(bar, host.firstChild);

    var tabs = panels.map(function (panel, k) {
      var tab = document.createElement('button');
      tab.type = 'button';
      tab.className = 'code-tab';
      tab.id = 'code-tab-' + g + '-' + k;
      tab.textContent = panel.querySelector('pre').getAttribute('data-lang');
      tab.setAttribute('role', 'tab');
      tab.setAttribute('aria-controls', tab.id + '-panel');
      panel.id = tab.id + '-panel';
      panel.setAttribute('role', 'tabpanel');
      panel.setAttribute('aria-labelledby', tab.id);
      tab.addEventListener('click', function () {
        remember(tab.textContent);
        choose(tab.textContent);
      });
      tab.addEventListener('keydown', function (event) {
        var next = { ArrowRight: k + 1, ArrowLeft: k - 1, Home: 0, End: tabs.length - 1 }[event.key];
        if (next === undefined) return;
        event.preventDefault();
        var target = tabs[(next + tabs.length) % tabs.length];
        target.focus();
        target.click();
      });
      bar.appendChild(tab);
      return tab;
    });

    function select(lang) {
      var k = tabs.map(function (tab) { return tab.textContent; }).indexOf(lang);
      if (k < 0) return;
      tabs.forEach(function (tab, j) {
        tab.setAttribute('aria-selected', String(j === k));
        tab.tabIndex = j === k ? 0 : -1;
        panels[j].hidden = j !== k;
      });
    }

    select(tabs[0].textContent);
    groups.push({ select: select });
  });

  if (remembered()) choose(remembered());
})();
