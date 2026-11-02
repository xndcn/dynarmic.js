// Progressive enhancements; benchmark execution stays in the lab controllers.
(() => {
  if (location.protocol === 'file:') {
    const guidance = document.createElement('p');
    guidance.id = 'file-guidance'; guidance.className = 'notice';
    guidance.setAttribute('role', 'alert');
    guidance.textContent = 'Serve the built dist directory over HTTP: run python3 -m http.server 8000 in that directory, then open http://localhost:8000/. Browser Workers and benchmark files cannot load from a file URL.';
    document.querySelector('main').prepend(guidance);
  }
  const themeButton = document.getElementById('theme-toggle');
  if (themeButton) {
    const themeLabel = document.getElementById('theme-label');
    const themeColor = document.querySelector('meta[name="theme-color"]');
    const dark = matchMedia('(prefers-color-scheme: dark)');
    const themes = ['system', 'light', 'dark'];
    let theme = document.documentElement.dataset.theme || 'system';
    const applyTheme = next => {
      theme = next;
      if (theme === 'system') delete document.documentElement.dataset.theme;
      else document.documentElement.dataset.theme = theme;
      try {
        if (theme === 'system') localStorage.removeItem('dynarmic-theme');
        else localStorage.setItem('dynarmic-theme', theme);
      } catch { /* The selected theme still applies for this page view. */ }
      const index = themes.indexOf(theme);
      const nextTheme = themes[(index + 1) % themes.length];
      themeLabel.textContent = theme[0].toUpperCase() + theme.slice(1);
      themeButton.setAttribute('aria-label', `Color theme: ${theme}. Change to ${nextTheme} theme.`);
      const resolved = theme === 'system' ? (dark.matches ? 'dark' : 'light') : theme;
      themeColor.content = resolved === 'dark' ? '#101b20' : '#f5f6f2';
      document.dispatchEvent(new CustomEvent('dynarmic-theme-change', {detail: {theme, resolved}}));
    };
    applyTheme(theme);
    themeButton.addEventListener('click', () => applyTheme(themes[(themes.indexOf(theme) + 1) % themes.length]));
    dark.addEventListener('change', () => { if (theme === 'system') applyTheme('system'); });
  }
  const engine = document.querySelector('.engine');
  const pipelineButton = document.getElementById('pipeline-toggle');
  if (engine && pipelineButton) {
    const reducedMotion = matchMedia('(prefers-reduced-motion: reduce)');
    let paused = false, visible = true;
    const updateMotion = () => {
      const animate = !paused && visible && !document.hidden && !reducedMotion.matches;
      engine.classList.toggle('is-animated', animate);
      pipelineButton.hidden = reducedMotion.matches;
      pipelineButton.textContent = paused ? 'Play motion' : 'Pause motion';
      pipelineButton.setAttribute('aria-pressed', String(paused));
      pipelineButton.setAttribute('aria-label', `${paused ? 'Play' : 'Pause'} motion: conceptual pipeline`);
    };
    pipelineButton.addEventListener('click', () => { paused = !paused; updateMotion(); });
    reducedMotion.addEventListener('change', updateMotion);
    document.addEventListener('visibilitychange', updateMotion);
    if ('IntersectionObserver' in globalThis) {
      new IntersectionObserver(([entry]) => { visible = entry.isIntersecting; updateMotion(); }, {rootMargin: '100px'}).observe(engine);
    }
    updateMotion();
  }
  const sectionLinks = [...document.querySelectorAll('.site-header nav a[href^="#"]')];
  const sections = sectionLinks.map(link => document.querySelector(link.getAttribute('href'))).filter(Boolean);
  let activeSection;
  const setActiveSection = current => {
    if (current === activeSection) return;
    activeSection = current;
    for (const link of sectionLinks) {
      if (link.getAttribute('href') === `#${current.id}`) link.setAttribute('aria-current', 'location');
      else link.removeAttribute('aria-current');
    }
  };
  if ('IntersectionObserver' in globalThis && sections.length) {
    // Observe a narrow reading band instead of forcing layout on every scroll.
    const observer = new IntersectionObserver(entries => {
      for (const entry of entries) if (entry.isIntersecting) setActiveSection(entry.target);
    }, {rootMargin: '-30% 0px -65% 0px', threshold: 0});
    for (const section of sections) observer.observe(section);
  } else if (sections.length) {
    const updateSection = () => {
      const marker = scrollY + innerHeight * .3;
      setActiveSection(sections.reduce((active, section) => section.offsetTop <= marker ? section : active, sections[0]));
    };
    addEventListener('scroll', updateSection, {passive: true});
    addEventListener('resize', updateSection, {passive: true});
    updateSection();
  }

  const stages = {
    decode: {
      label: '01 / Decode', title: "Start with the guest's instructions.",
      code: '; ARM32 guest instruction\nadd r0, r0, #1\n\n; Decoded operation, in pseudocode\nr0 ← r0 + 1',
      description: "Dynarmic's ARM32, Thumb and AArch64 frontends decode instructions into a shared intermediate representation. Optimizations work on that IR before the Wasm backend takes over.",
    },
    emit: {
      label: '02 / Emit', title: 'Turn optimized IR into WebAssembly.',
      code: ';; Simplified Wasm instruction sequence\nlocal.get $r0\ni32.const 1\ni32.add\nlocal.set $r0',
      description: 'The backend emits blocks and bounded regions as small Wasm modules. Guest registers stay in Wasm locals inside a region; generated modules share the host memory and function table.',
    },
    execute: {
      label: '03 / Execute', title: 'Compile once. Reuse the translation.',
      code: '// Dispatch flow, in pseudocode\nentry = cache.lookup(guestPC)\nif (!entry) entry = translate(guestPC)\n\nhaltBits = entry(state, context)',
      description: 'The JavaScript engine compiles and executes the emitted Wasm. Cached translations and linked blocks avoid repeating that work. The host must invalidate translations when guest instruction memory changes.',
    },
  };
  const buttons = [...document.querySelectorAll('[data-stage]')];
  for (const button of buttons) {
    button.disabled = false;
    button.addEventListener('click', () => {
      const stage = stages[button.dataset.stage];
      for (const other of buttons) other.setAttribute('aria-pressed', String(other === button));
      document.getElementById('stage-label').textContent = stage.label;
      const code = document.createElement('code');
      for (const [index, line] of stage.code.split('\n').entries()) {
        if (index) code.append('\n');
        if (line.startsWith(';') || line.startsWith('//')) {
          const comment = document.createElement('span');
          comment.className = 'code-comment'; comment.textContent = line;
          code.append(comment);
        } else code.append(line);
      }
      document.getElementById('stage-code').replaceChildren(code);
      document.getElementById('stage-title').textContent = stage.title;
      document.getElementById('stage-description').textContent = stage.description;
    });
  }
  const hints = {
    0: 'Callbacks route every guest memory access through the host. The host implements RAM, devices and access checks.',
    1: 'Page table uses mapped 4 KiB pages for direct access, with callbacks for unmapped or crossing accesses.',
    2: 'Flat window uses contiguous guest RAM with bounds checks and callbacks outside the window.',
  };
  const mode = document.getElementById('memory-mode');
  if (mode) {
    const update = () => { document.getElementById('mode-hint').textContent = hints[mode.value]; };
    mode.addEventListener('change', update);
    update();
  }
  const suite = document.getElementById('quickjs-suite');
  if (suite) {
    const running = () => !!(globalThis.quickjsRunning || globalThis.officialBenchRunning);
    const selectSuite = value => {
      if (running()) return;
      suite.value = value;
      document.getElementById('official-benchmarks').hidden = value !== 'official';
      document.getElementById('quickjs-custom').hidden = value !== 'custom';
    };
    suite.disabled = false;
    suite.addEventListener('change', () => selectSuite(suite.value));
    const followHash = () => {
      if (['#quickjs-custom', '#quickjs-results', '#quickjs-source'].includes(location.hash)) selectSuite('custom');
      else if (['#official-benchmarks', '#official-source'].includes(location.hash)) selectSuite('official');
    };
    const query = new URLSearchParams(location.search);
    selectSuite(query.get('quickjs') === '1' || query.get('quickjs-suite') === 'custom' ? 'custom' : 'official');
    followHash();
    window.addEventListener('hashchange', followHash);
    for (const event of ['quickjs-comparison-state', 'official-bench-state']) {
      document.addEventListener(event, () => { suite.disabled = running(); });
    }
  }
  let prismCore;
  const loadHighlighter = async language => {
    if (!['c', 'javascript'].includes(language)) throw new Error('Unsupported source language');
    if (!prismCore) {
      globalThis.Prism = {manual: true};
      prismCore = import('./vendor/prism-1.30.0/prism-core.js')
        .then(() => import('./vendor/prism-1.30.0/prism-clike.js'));
    }
    await prismCore;
    if (language === 'c') await import('./vendor/prism-1.30.0/prism-c.js');
    else await import('./vendor/prism-1.30.0/prism-javascript.js');
    return globalThis.Prism;
  };
  for (const viewer of document.querySelectorAll('[data-source]')) {
    const status = viewer.querySelector('.source-status');
    const retry = viewer.querySelector('.source-retry');
    const copy = viewer.querySelector('.source-copy');
    const code = viewer.querySelector('code');
    let loading = false, loaded = false;
    const fetchFile = async name => {
      const response = await fetch(name, {signal: AbortSignal.timeout(30000)});
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      return response;
    };
    const load = async () => {
      if (!viewer.open || loading || loaded) return;
      loading = true; retry.hidden = true;
      status.textContent = 'Loading benchmark source…';
      viewer.setAttribute('aria-busy', 'true');
      try {
        const source = await (await fetchFile(viewer.dataset.source)).text();
        if (!source.trim()) throw new Error('The source file is empty');
        // Show the original immediately. Loading a highlighter is an optional
        // enhancement and must not prevent reading or copying the source.
        code.textContent = source; copy.disabled = false;
        let highlighted = true;
        try {
          const prism = await loadHighlighter(viewer.dataset.language);
          code.className = `language-${viewer.dataset.language}`;
          // Prism escapes source markup before adding its own token spans.
          code.innerHTML = prism.highlight(source, prism.languages[viewer.dataset.language], viewer.dataset.language);
          if (code.textContent !== source) throw new Error('Highlighting changed the source text');
        }
        catch {
          code.textContent = source; highlighted = false;
        }
        status.textContent = `${viewer.dataset.source} · ${source.trimEnd().split('\n').length.toLocaleString('en-US')} lines${highlighted ? '' : ' · plain text'}`;
        loaded = true;
      } catch (error) {
        status.textContent = `Could not load the source (${error.message}). Retry or download the file above.`;
        retry.hidden = false;
      } finally {
        loading = false; viewer.setAttribute('aria-busy', 'false');
      }
    };
    viewer.addEventListener('toggle', load);
    retry.addEventListener('click', load);
    copy.addEventListener('click', async () => {
      try {
        await navigator.clipboard.writeText(code.textContent);
        status.textContent = 'Source code copied.';
      } catch {
        const range = document.createRange(); range.selectNodeContents(code);
        const selection = window.getSelection(); selection.removeAllRanges(); selection.addRange(range);
        status.textContent = 'Clipboard unavailable. Source selected; copy it with your keyboard or selection menu.';
      }
    });
    if (viewer.open) load();
  }
  for (const button of document.querySelectorAll('[data-copy]')) {
    button.disabled = false;
    button.addEventListener('click', async () => {
      const code = document.getElementById(button.dataset.copy);
      const status = document.getElementById('copy-status');
      try {
        await navigator.clipboard.writeText(code.textContent);
        status.textContent = `${button.dataset.copyLabel || 'Code'} copied.`;
        button.textContent = 'Copied';
        setTimeout(() => { button.textContent = 'Copy'; }, 1800);
      } catch {
        const range = document.createRange();
        range.selectNodeContents(code);
        const selection = window.getSelection();
        selection.removeAllRanges(); selection.addRange(range);
        status.textContent = 'Clipboard unavailable. The code is selected; copy it with your keyboard or selection menu.';
      }
    });
  }
})();
