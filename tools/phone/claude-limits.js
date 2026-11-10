// Bookmarklet for Chrome on Android: run it on https://claude.ai/settings/usage
// with the watch plugged into the phone (USB-C). It reads the usage shown for
// the signed-in account and sends it to the watch's serial console (command
// 'A', same as `watchctl.py aisync`). Nothing leaves the phone except over USB.
// Build the bookmark URL with: python tools/phone/make_bookmarklet.py
(async () => {
  const now = Math.floor(Date.now() / 1000);
  const free = used => Math.max(0, Math.min(100, Math.round(100 - used)));   // the watch stores "remaining"

  // "3 hr 12 min" / "3 Std. 12 Min." -> epoch seconds, 0 if not found
  const relReset = t => {
    const h = /(\d+)\s*(?:hr|h\b|hour|Std)/i.exec(t), m = /(\d+)\s*(?:min)/i.exec(t);
    return h || m ? now + (h ? +h[1] * 3600 : 0) + (m ? +m[1] * 60 : 0) : 0;
  };

  // Page text: first "NN% used" is the 5-hour session, second the weekly limit.
  // shortcut: the weekly reset is a weekday on the page and stays unknown here; the API path below has it.
  const fromText = text => {
    const re = /(\d{1,3})\s*%\s*(?:used|verwendet|genutzt|verbraucht)/gi;
    const hits = [];
    for (let m; (m = re.exec(text));) hits.push({ used: +m[1], at: m.index });
    if (!hits.length) return null;
    const o = { c_5h_p: free(hits[0].used), c_5h_r: relReset(text.slice(Math.max(0, hits[0].at - 120), hits[0].at + 40)) };
    if (hits[1]) o.c_wk_p = free(hits[1].used);
    return o;
  };

  // The page's own data source (undocumented, may change): exact reset times.
  const fromApi = async () => {
    const org = (document.cookie.match(/lastActiveOrg=([^;]+)/) || [])[1];
    if (!org) return null;
    const r = await fetch(`/api/organizations/${org}/usage`, { credentials: 'include' });
    if (!r.ok) return null;
    const j = await r.json(), o = {};
    const put = (win, p, rs) => {
      if (!win || typeof win.utilization !== 'number') return;
      o[p] = free(win.utilization);
      const t = Date.parse(win.resets_at) / 1000;
      if (t > now) o[rs] = Math.floor(t);
    };
    put(j.five_hour, 'c_5h_p', 'c_5h_r');
    put(j.seven_day, 'c_wk_p', 'c_wk_r');
    return 'c_5h_p' in o ? o : null;
  };

  // USB CDC-ACM: interface class 2 = control, class 10 = data (bulk in/out)
  const send = async line => {
    const known = await navigator.usb.getDevices();
    const dev = known.find(d => d.vendorId === 0x303A) || await navigator.usb.requestDevice({ filters: [{ vendorId: 0x303A }] });
    await dev.open();
    try {
      if (!dev.configuration) await dev.selectConfiguration(1);
      const ifs = dev.configuration.interfaces, cls = c => ifs.find(i => i.alternate.interfaceClass === c);
      const ctl = cls(2), data = cls(10);
      if (!data) throw new Error('keine serielle Schnittstelle');
      if (ctl) {
        await dev.claimInterface(ctl.interfaceNumber);
        // DTR on: the watch only answers while the port counts as open
        await dev.controlTransferOut({ requestType: 'class', recipient: 'interface', request: 0x22, value: 1, index: ctl.interfaceNumber });
      }
      await dev.claimInterface(data.interfaceNumber);
      const ep = d => data.alternate.endpoints.find(e => e.direction === d && e.type === 'bulk').endpointNumber;
      await dev.transferOut(ep('out'), new TextEncoder().encode(line + '\n'));
      let got = '';
      for (const end = Date.now() + 3000; Date.now() < end && !/AI_LIMITS_SET ok=\d/.test(got);) {
        const r = await Promise.race([dev.transferIn(ep('in'), 512), new Promise(res => setTimeout(res, 1000))]);
        if (r && r.data) got += new TextDecoder().decode(r.data);
      }
      return /AI_LIMITS_SET ok=1/.test(got);
    } finally {
      await dev.close();
    }
  };

  if (typeof document === 'undefined') { module.exports = { fromText, free }; return; }   // node self-check

  let v = null;
  try { v = await fromApi(); } catch (e) { /* fall back to the visible text */ }
  v = v || fromText(document.body.innerText);

  const box = document.createElement('div');
  box.style.cssText = 'position:fixed;left:12px;right:12px;bottom:12px;z-index:99999;background:#111;color:#fff;padding:16px;border-radius:14px;font:16px sans-serif';
  const time = t => t ? new Date(t * 1000).toLocaleString([], { weekday: 'short', hour: '2-digit', minute: '2-digit' }) : 'unbekannt';
  const msg = document.createElement('div');
  msg.textContent = !v ? 'Keine Nutzungswerte gefunden. Bitte die Seite Einstellungen > Nutzung öffnen.'
    : `5 Stunden: ${v.c_5h_p} % frei, Reset ${time(v.c_5h_r)}` + ('c_wk_p' in v ? ` · Woche: ${v.c_wk_p} % frei, Reset ${time(v.c_wk_r)}` : '');
  const btn = (label, fn) => {
    const b = document.createElement('button');
    b.textContent = label;
    b.style.cssText = 'margin:12px 8px 0 0;padding:12px 16px;border:0;border-radius:10px;font:inherit;min-height:48px';
    b.onclick = fn;
    box.append(b);
  };
  box.append(msg);
  if (v) btn('An Uhr senden', async () => {
    try {
      msg.textContent = (await send('A' + JSON.stringify({ ...v, sync: now }))) ? 'Auf der Uhr gespeichert.' : 'Gesendet, aber keine Bestätigung von der Uhr.';
    } catch (e) {
      msg.textContent = 'USB-Fehler: ' + e.message;
    }
  });
  btn('Schließen', () => box.remove());
  document.body.append(box);
})();
