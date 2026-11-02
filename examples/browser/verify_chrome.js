// Headless-Chrome check for an ARM host test fixture, driven over DevTools:
//   chrome --headless=new --no-sandbox --remote-debugging-port=9333 about:blank &
//   node verify_chrome.js 'http://127.0.0.1:8000/arm-smoke.html?auto=1' 9333
// Polls document.title until it starts with DONE (PASS/FAIL); exit 0 on PASS.
const [url, port, ...extra] = process.argv.slice(2);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
(async () => {
  if (!url || !/^https?:\/\//.test(url) || !/^\d+$/.test(port || '') ||
      Number(port) < 1 || Number(port) > 65535 || extra.length) {
    throw new Error('usage: node verify_chrome.js <http(s)-page-url> <devtools-port>');
  }
  let page;
  const discoveryDeadline = Date.now() + 10000;
  while (Date.now() < discoveryDeadline) {
    try {
      const response = await fetch(`http://127.0.0.1:${port}/json/list`, {signal: AbortSignal.timeout(1000)});
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const targets = await response.json();
      if (Array.isArray(targets)) page = targets.find((target) => target?.type === 'page' && target.webSocketDebuggerUrl);
      if (page) break;
    } catch {}
    await sleep(200);
  }
  if (!page) throw new Error(`Chrome DevTools endpoint ${port} did not expose a page target`);
  const ws = new WebSocket(page.webSocketDebuggerUrl);
  await new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('timed out connecting to Chrome DevTools')), 10000);
    ws.onopen = () => { clearTimeout(timer); resolve(); };
    ws.onerror = () => { clearTimeout(timer); reject(new Error('Chrome DevTools WebSocket failed')); };
    ws.onclose = () => { clearTimeout(timer); reject(new Error('Chrome DevTools closed before connecting')); };
  });
  let id = 0; const pending = new Map();
  ws.onmessage = (m) => {
    let d;
    try { d = JSON.parse(m.data); } catch { return; }
    if (d.id && pending.has(d.id)) { pending.get(d.id).resolve(d); pending.delete(d.id); }
  };
  const disconnect = () => {
    for (const {reject} of pending.values()) reject(new Error('Chrome DevTools connection closed or failed'));
    pending.clear();
  };
  ws.onerror = disconnect;
  ws.onclose = disconnect;
  const call = (method, params = {}) => new Promise((resolve, reject) => {
    const i = ++id;
    const timer = setTimeout(() => { pending.delete(i); reject(new Error(`Chrome DevTools timeout: ${method}`)); }, 10000);
    pending.set(i, {
      resolve: (value) => {
        clearTimeout(timer);
        if (value.error) reject(new Error(`Chrome DevTools ${method}: ${value.error.message || JSON.stringify(value.error)}`));
        else resolve(value);
      },
      reject: (error) => { clearTimeout(timer); reject(error); },
    });
    try { ws.send(JSON.stringify({id: i, method, params})); }
    catch (error) { clearTimeout(timer); pending.delete(i); reject(error); }
  });
  await call('Page.enable');
  // Repeated checks reuse a tab. Page.navigate can return before the new
  // document commits, so do not mistake the previous page's DONE for this run.
  await call('Runtime.evaluate', {expression: "document.title = 'Loading demo guests'"});
  const navigation = await call('Page.navigate', {url});
  if (navigation.result?.errorText) throw new Error(`Chrome navigation failed: ${navigation.result.errorText}`);
  const evalJs = async (expr) => {
    const response = await call('Runtime.evaluate', {expression: expr, returnByValue: true});
    if (response.result?.exceptionDetails) {
      throw new Error(`Chrome page evaluation failed: ${response.result.exceptionDetails.text}`);
    }
    return response.result?.result?.value;
  };
  const t0 = Date.now();
  while (Date.now() - t0 < 120000) {
    await sleep(500);
    const title = await evalJs('document.title');
    if (title && title.startsWith('DONE')) {
      console.log('title:', title);
      console.log('userAgent:', await evalJs('navigator.userAgent'));
      console.log('--- output ---\n' + await evalJs("[0,1].map(i => document.getElementById('out' + i).textContent).join('---\\n')"));
      console.log('--- status ---\n' + await evalJs("document.getElementById('status').textContent"));
      console.log('--- stats ---\n' + await evalJs("[0,1].map(a => [...document.querySelectorAll('#stats' + a + ' dt')].map((d,i)=>d.textContent+': '+document.querySelectorAll('#stats' + a + ' dd')[i].textContent).join('\\n')).join('\\n---\\n')"));
      ws.close(); process.exit(title === 'DONE PASS' ? 0 : 1);
    }
  }
  console.log('timeout; last status:', await evalJs("document.getElementById('status')?.textContent || '(page did not load)'"));
  ws.close(); process.exit(2);
})().catch((error) => {
  console.error(error.message);
  process.exit(1);
});
