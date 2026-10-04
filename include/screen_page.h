#pragma once

// Web page shared by the screen demo (tft_web) and the device firmware (device):
// the phone renders text, photos and document pages to a 240x320 canvas and uploads them.
static const char *SCREEN_PAGE_HTML = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Gửi lên màn hình ESP32</title>
  <style>
    body { font-family: sans-serif; margin: 0; background: #f5f5f5; }
    .wrap { max-width: 480px; margin: 0 auto; padding: 16px; }
    h1 { font-size: 21px; }
    h2 { font-size: 16px; margin: 0 0 8px; }
    .card { background: #fff; border-radius: 10px; padding: 12px 14px; box-shadow: 0 1px 2px #0002; margin-bottom: 14px; }
    textarea, input[type=file] { width: 100%; box-sizing: border-box; font-size: 16px; padding: 8px; border: 1px solid #ccc; border-radius: 8px; }
    button { font-size: 16px; padding: 12px; border: none; border-radius: 8px; background: #1e88e5; color: #fff; width: 100%; margin-top: 8px; }
    button.grey { background: #757575; }
    button.green { background: #43a047; }
    .row { display: flex; gap: 10px; align-items: center; flex-wrap: wrap; margin: 8px 0; font-size: 14px; color: #555; }
    .row button { width: auto; flex: 1; margin-top: 0; }
    input[type=color] { width: 44px; height: 32px; border: none; padding: 0; }
    canvas { width: 180px; height: 240px; border: 1px solid #ccc; background: #000; display: block; margin: 8px auto; }
    #msg, #docInfo, #boardInfo { color: #555; font-size: 14px; min-height: 1.2em; }
  </style>
</head>
<body>
<div class="wrap">
  <h1>Gửi lên màn hình ESP32</h1>

  <div class="card" id="navCard">
    <h2>Chỉ đường</h2>
    <textarea id="navLink" rows="2" placeholder="Dán link Google Maps (Chia sẻ → Sao chép liên kết) hoặc tọa độ, vd 21.0285, 105.8522"></textarea>
    <button id="navSet">Đặt điểm đến</button>
    <div id="navInfo" class="meta"></div>
  </div>

  <div class="card" id="bgCard" hidden>
    <h2>Hình nền đồng hồ</h2>
    <input type="file" id="bgFile" accept="image/*">
    <div class="row"><label><input type="checkbox" id="bgDim" checked> Làm tối ảnh để chữ dễ đọc</label></div>
    <div class="row">
      <button id="bgSet" class="green">Đặt làm hình nền</button>
      <button id="bgClear" class="grey">Bỏ hình nền</button>
    </div>
    <div id="bgInfo" class="meta"></div>
  </div>

  <div class="card" id="askCard" hidden>
    <h2>Hỏi AI bằng giọng nói</h2>
    <div class="row">Nền tảng
      <select id="askProvider" style="flex:1;font-size:16px;padding:6px">
        <option value="gemini">Gemini (Google)</option>
        <option value="groq">Groq (Whisper + chat)</option>
      </select>
    </div>
    <div class="row">
      <button id="askBtn">Bắt đầu hỏi</button>
      <button id="askCancel" class="grey" hidden>Hủy</button>
    </div>
    <div id="askInfo" class="meta"></div>
    <div id="askQ" style="color:#1565c0;margin-top:8px;white-space:pre-wrap"></div>
    <div id="askA" style="margin-top:6px;white-space:pre-wrap"></div>
    <details id="askSetup" style="margin-top:10px">
      <summary id="askSetupTitle">Khóa API</summary>
      <input id="askKey" type="password" autocomplete="off" placeholder="Dán khóa API" style="width:100%;box-sizing:border-box;font-size:16px;padding:8px;margin-top:8px">
      <input id="askModel" placeholder="Model" style="width:100%;box-sizing:border-box;font-size:16px;padding:8px;margin-top:8px">
      <button id="askSave" class="grey">Lưu</button>
    </details>
  </div>

  <div class="card" id="recCard" hidden>
    <h2>Ghi âm</h2>
    <button id="recBtn">Bắt đầu ghi</button>
    <div id="recInfo" class="meta"></div>
    <audio id="recPlayer" controls hidden style="width:100%;margin-top:8px"></audio>
    <div id="recList"></div>
  </div>

  <div class="card">
    <h2>Chữ</h2>
    <textarea id="txt" rows="3" placeholder="Nhập chữ, có dấu cũng được..."></textarea>
    <div class="row">
      <label>Cỡ chữ <input type="range" id="size" min="1" max="6" value="4"></label>
      <label>Màu chữ <input type="color" id="fg" value="#ffffff"></label>
      <label>Nền <input type="color" id="bg" value="#000000"></label>
    </div>
    <button id="sendText">Hiện chữ lên màn hình</button>
  </div>

  <div class="card">
    <h2>Ảnh</h2>
    <input type="file" id="file" accept="image/*">
    <button id="sendImg">Gửi ảnh lên màn hình</button>
  </div>

  <div class="card">
    <h2>Tài liệu (.txt, .pdf, .docx)</h2>
    <input type="file" id="doc" accept=".txt,.pdf,.docx,text/plain,application/pdf">
    <input type="text" id="docName" placeholder="Tên tài liệu (hiện trên board)">
    <div id="docInfo">Chưa mở tài liệu nào.</div>
    <div class="row">
      <button id="prev" class="grey">Trang trước</button>
      <button id="next" class="grey">Trang sau</button>
    </div>
    <button id="saveDoc" class="green">Lưu cả tài liệu vào board (lật bằng nút BOOT)</button>
  </div>

  <div class="card">
    <h2>Tài liệu đang có trong board</h2>
    <div id="boardList">Đang kiểm tra...</div>
    <div id="boardInfo"></div>
    <div class="row">
      <button id="bPrev" class="grey">Trang trước</button>
      <button id="bNext" class="grey">Trang sau</button>
    </div>
  </div>

  <h2>Xem trước</h2>
  <canvas id="cv" width="240" height="320"></canvas>
  <button id="clear" class="grey">Xóa màn hình</button>
  <div id="msg"></div>
</div>
<script>
  const $ = id => document.getElementById(id);
  const cv = $('cv'), ctx = cv.getContext('2d', { willReadFrequently: true });
  ctx.fillStyle = '#000'; ctx.fillRect(0, 0, 240, 320);
  let docPages = [], docPage = 0, docText = '';
  function say(text, bad) {
    $('msg').textContent = text;
    $('msg').style.background = bad ? '#ffcdd2' : 'transparent';
    $('msg').style.padding = bad ? '8px' : '0';
    $('docInfo').textContent = bad ? text : $('docInfo').textContent;
  }

  function fontPx() { return [12, 16, 20, 26, 34, 44][$('size').value - 1]; }
  function wrap(text, px) {
    ctx.font = px + 'px sans-serif';
    const lines = [];
    for (const para of text.split('\n')) {
      let line = '';
      for (const word of para.split(/\s+/)) {
        if (!word) continue;
        const next = line ? line + ' ' + word : word;
        if (line && ctx.measureText(next).width > 240 - 16) { lines.push(line); line = word; }
        else line = next;
      }
      lines.push(line);
    }
    return lines;
  }
  function paint(lines, px) {
    ctx.fillStyle = $('bg').value; ctx.fillRect(0, 0, 240, 320);
    ctx.fillStyle = $('fg').value; ctx.textBaseline = 'top';
    ctx.font = px + 'px sans-serif';
    const lineH = px * 1.25, top = Math.max(6, (320 - lines.length * lineH) / 2);
    lines.forEach((l, i) => ctx.fillText(l, 8, top + i * lineH));
  }
  function drawText() {
    let px = fontPx(), lines = wrap($('txt').value, px);
    while (px > 8 && lines.length * px * 1.25 > 320 - 12) { px -= 2; lines = wrap($('txt').value, px); }
    paint(lines, px);
  }
  // Split the document into screen-sized pages at the chosen font size.
  function paginate(text, keepPosition) {
    docText = text;
    const px = fontPx(), lineH = px * 1.25, perPage = Math.max(1, Math.floor((320 - 12) / lineH));
    const lines = wrap(text, px);
    const fraction = keepPosition && docPages.length ? docPage / docPages.length : 0;
    docPages = [];
    for (let i = 0; i < lines.length; i += perPage) docPages.push(lines.slice(i, i + perPage));
    showDocPage(Math.floor(fraction * docPages.length)); // stay roughly where the reader was
  }
  function showDocPage(i) {
    if (!docPages.length) return;
    docPage = Math.max(0, Math.min(i, docPages.length - 1));
    paint(docPages[docPage], fontPx());
    $('docInfo').textContent = 'Trang ' + (docPage + 1) + '/' + docPages.length +
      '. Cỡ chữ nhỏ hơn thì ít trang hơn.';
  }

  function rgb(hex) { return [1, 3, 5].map(i => parseInt(hex.substr(i, 2), 16)); }
  function to565(hex) { const [r, g, b] = rgb(hex); return ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3); }
  function toRGB565Blob(name) {
    const d = ctx.getImageData(0, 0, 240, 320).data;
    const buf = new Uint16Array(240 * 320); // little-endian: the board reads the low byte first
    for (let i = 0, p = 0; i < buf.length; i++, p += 4)
      buf[i] = ((d[p] & 0xf8) << 8) | ((d[p + 1] & 0xfc) << 3) | (d[p + 2] >> 3);
    const fd = new FormData();
    fd.append('img', new Blob([buf.buffer]), name);
    return fd;
  }
  // Text pages: 1 bit per pixel plus the two colors, 16x smaller than RGB565.
  function toMonoBlob(name) {
    const d = ctx.getImageData(0, 0, 240, 320).data;
    const fg = rgb($('fg').value), bg = rgb($('bg').value);
    const out = new Uint8Array(4 + 240 * 320 / 8);
    const dv = new DataView(out.buffer);
    dv.setUint16(0, to565($('fg').value), true);
    dv.setUint16(2, to565($('bg').value), true);
    for (let i = 0, p = 0; i < 240 * 320; i++, p += 4) {
      const df = (d[p] - fg[0]) ** 2 + (d[p + 1] - fg[1]) ** 2 + (d[p + 2] - fg[2]) ** 2;
      const db = (d[p] - bg[0]) ** 2 + (d[p + 1] - bg[1]) ** 2 + (d[p + 2] - bg[2]) ** 2;
      if (df < db) out[4 + (i >> 3)] |= 0x80 >> (i & 7);
    }
    const fd = new FormData();
    fd.append('img', new Blob([out.buffer]), name);
    return fd;
  }
  function send(url, name, mono) {
    const body = mono ? toMonoBlob(name) : toRGB565Blob(name || 'img.bin');
    return fetch(url, { method: 'POST', body }).then(r => r.text());
  }
  function sendCurrent() { say('Đang gửi...'); send('/image').then(t => say(t)).catch(e => say('Lỗi: ' + e, true)); }

  // Text extraction. PDF and DOCX use libraries loaded from the internet on demand.
  function loadScript(src) {
    return new Promise((ok, err) => { const s = document.createElement('script'); s.src = src; s.onload = ok; s.onerror = () => err(new Error('không tải được ' + src)); document.head.append(s); });
  }
  async function extract(file) {
    const name = file.name.toLowerCase();
    if (name.endsWith('.pdf')) {
      if (!window.pdfjsLib) {
        await loadScript('https://cdnjs.cloudflare.com/ajax/libs/pdf.js/3.11.174/pdf.min.js');
        pdfjsLib.GlobalWorkerOptions.workerSrc = 'https://cdnjs.cloudflare.com/ajax/libs/pdf.js/3.11.174/pdf.worker.min.js';
      }
      const pdf = await pdfjsLib.getDocument({ data: await file.arrayBuffer() }).promise;
      let text = '';
      for (let p = 1; p <= pdf.numPages; p++) {
        const tc = await (await pdf.getPage(p)).getTextContent();
        text += tc.items.map(it => it.str + (it.hasEOL ? '\n' : ' ')).join('') + '\n';
      }
      return text;
    }
    if (name.endsWith('.docx')) {
      if (!window.mammoth) await loadScript('https://cdnjs.cloudflare.com/ajax/libs/mammoth/1.6.0/mammoth.browser.min.js');
      return (await mammoth.extractRawText({ arrayBuffer: await file.arrayBuffer() })).value;
    }
    return await file.text();
  }
  $('doc').onchange = async () => {
    const f = $('doc').files[0];
    if (!f) return;
    if (!$('docName').value.trim()) $('docName').value = f.name.replace(/\.[^.]+$/, '').substring(0, 40);
    $('docInfo').textContent = 'Đang đọc tài liệu...';
    try {
      const text = await extract(f);
      if (!text.trim()) { $('docInfo').textContent = 'Không lấy được chữ nào (file ảnh scan?).'; return; }
      paginate(text);
      $('msg').textContent = 'Đã chia ' + docPages.length + ' trang. Bấm "Trang sau" để xem, hoặc lưu vào board.';
    } catch (e) {
      $('docInfo').textContent = 'Lỗi đọc tài liệu: ' + e.message + ' (file .pdf và .docx cần điện thoại có internet)';
    }
  };
  $('prev').onclick = () => { showDocPage(docPage - 1); sendCurrent(); };
  $('next').onclick = () => { showDocPage(docPage + 1); sendCurrent(); };
  $('saveDoc').onclick = async () => {
    if (!docPages.length) { say('Chưa mở tài liệu', true); return; }
    const total = docPages.length;
    try {
      const name = $('docName').value.trim() || 'Tai lieu';
      const r = await fetch('/doc/begin?count=' + total + '&mono=1&name=' + encodeURIComponent(name), { method: 'POST' });
      if (!r.ok) { const t = await r.text(); say(t, true); alert(t); return; }
      const t0 = Date.now();
      for (let i = 0; i < total; i++) {
        paint(docPages[i], fontPx());
        const left = i ? Math.round((Date.now() - t0) / i * (total - i) / 1000) : 0;
        say('Đang lưu trang ' + (i + 1) + '/' + total + (left ? ' (còn khoảng ' + left + ' giây)' : ''));
        const res = await send('/page', 'm' + i + '.bin', true);
        if (!res.startsWith('ok')) { say('Lỗi ở trang ' + (i + 1) + ': ' + res, true); alert(res); return; }
      }
      await fetch('/doc/end', { method: 'POST' });
      say('Đã lưu ' + total + ' trang vào board. Bấm nút BOOT để sang trang, giữ 1 giây để lùi trang.');
      showDocPage(docPage);
      boardInfo();
    } catch (e) {
      say('Lỗi khi lưu: ' + e.message, true);
      alert('Lỗi khi lưu: ' + e.message);
    }
  };

  function boardInfo() {
    return fetch('/docs').then(r => r.ok ? r.json() : Promise.reject()).then(d => {
      const box = $('boardList');
      box.textContent = d.docs.length ? '' : 'Board chưa có tài liệu nào.';
      d.docs.forEach(doc => {
        const row = document.createElement('div');
        row.className = 'row';
        const label = document.createElement('span');
        label.style.flex = '1';
        label.textContent = doc.name + ' · ' + doc.pages + ' trang' +
          (doc.reading ? ' · đang đọc trang ' + (doc.page + 1) : '');
        const open = document.createElement('button');
        open.textContent = 'Đọc';
        open.onclick = () => post('/doc/open', { slot: doc.slot });
        const del = document.createElement('button');
        del.textContent = 'Xóa';
        del.className = 'grey';
        del.onclick = () => { if (confirm('Xóa "' + doc.name + '" khỏi board?')) post('/doc/delete', { slot: doc.slot }); };
        row.append(label, open, del);
        box.append(row);
      });
      $('boardInfo').textContent = 'Còn trống ' + Math.round(d.free / 1048576) + ' MB, chứa tối đa ' + d.max + ' tài liệu.';
    }).catch(() => { $('boardList').textContent = 'Không đọc được danh sách tài liệu.'; });
  }
  function post(url, data) { // for the board buttons: send, show the reply, refresh the list
    return fetch(url, { method: 'POST', body: new URLSearchParams(data) })
      .then(r => r.text()).then(t => { say(t); boardInfo(); });
  }
  $('bPrev').onclick = () => post('/doc/step?d=-1', {});
  $('bNext').onclick = () => post('/doc/step?d=1', {});

  $('file').onchange = async () => {
    const f = $('file').files[0];
    if (!f) return;
    const bmp = await createImageBitmap(f, { imageOrientation: 'from-image' }); // honours EXIF rotation
    ctx.fillStyle = '#000'; ctx.fillRect(0, 0, 240, 320);
    const s = Math.min(240 / bmp.width, 320 / bmp.height);
    ctx.drawImage(bmp, (240 - bmp.width * s) / 2, (320 - bmp.height * s) / 2, bmp.width * s, bmp.height * s);
    $('msg').textContent = 'Đã xem trước. Bấm "Gửi ảnh lên màn hình".';
  };
  $('sendText').onclick = () => { drawText(); sendCurrent(); };
  $('sendImg').onclick = () => { if (!$('file').files[0]) { $('msg').textContent = 'Chưa chọn ảnh'; return; } sendCurrent(); };
  $('clear').onclick = () => {
    ctx.fillStyle = '#000'; ctx.fillRect(0, 0, 240, 320);
    fetch('/clear', { method: 'POST' }).then(r => r.text()).then(t => $('msg').textContent = t);
  };
  ['txt', 'size', 'fg', 'bg'].forEach(id => $(id).addEventListener('input',
    () => docText ? paginate(docText, true) : drawText()));
  boardInfo();

  function navInfo() {
    fetch('/nav').then(r => r.ok ? r.json() : Promise.reject()).then(d => {
      const dist = d.distanceM < 0 ? '' : ' · còn ' + (d.distanceM >= 1000 ? (d.distanceM / 1000).toFixed(1) + ' km' : d.distanceM + ' m');
      $('navInfo').textContent = d.hasDest
        ? 'Điểm đến: ' + (d.dest || 'không tên') + dist + (d.hasFix ? ' · GPS ' + d.fixAgeS + ' giây trước' : ' · chưa có GPS')
        : 'Chưa có điểm đến.';
    }).catch(() => { $('navCard').hidden = true; }); // firmware without navigation
  }
  $('navSet').onclick = () => {
    const link = $('navLink').value.trim();
    if (!link) return;
    say('Đang đọc link...');
    fetch('/nav/dest', { method: 'POST', body: new URLSearchParams({ link }) })
      .then(r => r.text().then(t => {
        say(r.ok ? 'Đã đặt điểm đến' : t, !r.ok);
        if (r.ok) $('navLink').value = '';
        navInfo();
      }));
  };
  navInfo();
  setInterval(navInfo, 5000);

  // Recordings: iPhone Safari won't play audio from a server without range requests, so a
  // recording is fetched whole and played from a blob.
  let recOn = false, recNames = '';
  const mmss = s => String(Math.floor(s / 60)).padStart(2, '0') + ':' + String(Math.floor(s % 60)).padStart(2, '0');
  function recInfo() {
    fetch('/rec').then(r => r.ok ? r.json() : Promise.reject()).then(d => {
      $('recCard').hidden = false;
      recOn = d.recording;
      $('recBtn').textContent = recOn ? 'Dừng và lưu' : 'Bắt đầu ghi';
      $('recBtn').style.background = recOn ? '#e53935' : '';
      $('recInfo').textContent = (recOn ? 'Đang ghi ' + mmss(d.elapsedS) + ' · ' : '') + 'Còn trống khoảng ' + mmss(d.freeS);
      const names = d.files.map(f => f.name).join();
      if (names === recNames) return; // don't rebuild the list (and stop playback) for nothing
      recNames = names;
      const box = $('recList');
      box.textContent = d.files.length ? '' : 'Chưa có bản ghi nào.';
      d.files.forEach(f => {
        const row = document.createElement('div');
        row.className = 'row';
        const label = document.createElement('span');
        label.style.flex = '1 1 100%';
        label.textContent = f.name.replace('.wav', '') + ' · ' + mmss(f.seconds);
        const play = document.createElement('button');
        play.textContent = 'Nghe';
        play.onclick = () => {
          say('Đang tải ' + f.name + '...');
          fetch('/rec/file?name=' + encodeURIComponent(f.name)).then(r => r.blob()).then(b => {
            const p = $('recPlayer');
            if (p.src) URL.revokeObjectURL(p.src);
            p.src = URL.createObjectURL(b);
            p.hidden = false;
            p.play();
            say('Đang phát ' + f.name);
          });
        };
        const dl = document.createElement('button');
        dl.textContent = 'Tải về';
        dl.className = 'green';
        dl.onclick = () => {
          const a = document.createElement('a');
          a.href = '/rec/file?dl=1&name=' + encodeURIComponent(f.name);
          a.download = f.name;
          a.click();
        };
        const del = document.createElement('button');
        del.textContent = 'Xóa';
        del.className = 'grey';
        del.onclick = () => {
          if (!confirm('Xóa bản ghi ' + f.name + '?')) return;
          fetch('/rec/delete', { method: 'POST', body: new URLSearchParams({ name: f.name }) })
            .then(r => r.text()).then(t => { say(t); recInfo(); });
        };
        row.append(label, play, dl, del);
        box.append(row);
      });
    }).catch(() => {}); // firmware without a recorder: the card stays hidden
  }
  $('recBtn').onclick = () => fetch(recOn ? '/rec/stop' : '/rec/start', { method: 'POST' })
    .then(r => r.text()).then(t => { say(t); recInfo(); });
  recInfo();
  setInterval(recInfo, 2000);

  let askState = '', askShownProvider = '';
  const ASK_KEY_HINT = { gemini: 'Dán khóa Gemini (aistudio.google.com → Get API key)', groq: 'Dán khóa Groq (console.groq.com → API Keys)' };
  function askInfo() {
    fetch('/ask').then(r => r.ok ? r.json() : Promise.reject()).then(d => {
      $('askCard').hidden = false;
      askState = d.state;
      if (d.provider !== askShownProvider) { // provider changed (here or with the knob): refresh its fields
        askShownProvider = d.provider;
        $('askProvider').value = d.provider;
        $('askModel').value = d.model;
        $('askKey').placeholder = ASK_KEY_HINT[d.provider] || 'Dán khóa API';
        $('askSetupTitle').textContent = 'Khóa API ' + $('askProvider').selectedOptions[0].textContent + (d.hasKey ? ' (đã có)' : '');
      }
      if (!d.hasKey) $('askSetup').open = true;
      $('askBtn').textContent = d.state === 'recording' ? 'Dừng và gửi' : d.state === 'thinking' ? 'Đang chờ trả lời...' : 'Bắt đầu hỏi';
      $('askBtn').style.background = d.state === 'recording' ? '#e53935' : '';
      $('askBtn').disabled = d.state === 'thinking' || !d.hasKey;
      $('askCancel').hidden = !(d.state === 'recording' || d.state === 'thinking');
      $('askInfo').textContent = !d.hasKey ? 'Chưa có khóa API.'
        : d.state === 'recording' ? 'Đang nghe... ' + d.elapsedS + ' giây'
        : d.state === 'thinking' ? 'Gemini đang trả lời... ' + d.elapsedS + ' giây'
        : d.state === 'failed' ? 'Lỗi: ' + d.error
        : d.state === 'answer' ? 'Trả lời sau ' + (d.tookMs / 1000).toFixed(1) + ' giây' : '';
      // the last answer stays visible here after the board's screen went back to Ready
      const showLast = d.state === 'answer' || d.state === 'idle';
      $('askQ').textContent = showLast && d.question ? 'Bạn: ' + d.question : '';
      $('askA').textContent = showLast ? d.answer : '';
    }).catch(() => {}); // firmware without Ask AI: the card stays hidden
  }
  $('askBtn').onclick = () => fetch('/ask/press', { method: 'POST' }).then(askInfo);
  $('askCancel').onclick = () => fetch('/ask/cancel', { method: 'POST' }).then(r => r.text()).then(t => { say(t); askInfo(); });
  $('askProvider').onchange = () => fetch('/ask/key', { method: 'POST', body: new URLSearchParams({ provider: $('askProvider').value }) })
    .then(r => r.text()).then(t => { say(t); askInfo(); });
  $('askSave').onclick = () => {
    const data = { provider: $('askProvider').value, model: $('askModel').value.trim() };
    if ($('askKey').value.trim()) data.key = $('askKey').value.trim();
    fetch('/ask/key', { method: 'POST', body: new URLSearchParams(data) }).then(r => r.text()).then(t => {
      say(t);
      $('askKey').value = '';
      $('askSetup').open = false;
      askShownProvider = ''; // re-read the "(đã có)" state
      askInfo();
    });
  };
  askInfo();
  setInterval(askInfo, 1500);

  // Clock background: the photo is cropped to fill the screen (not letterboxed) and drawn on the
  // preview canvas, which is what gets sent.
  function bgInfo() {
    fetch('/clockbg').then(r => r.ok ? r.json() : Promise.reject()).then(d => {
      $('bgCard').hidden = false;
      $('bgInfo').textContent = d.set ? 'Đồng hồ đang dùng ảnh nền.' : 'Đồng hồ đang dùng nền đen.';
    }).catch(() => {});
  }
  async function bgDraw() {
    const f = $('bgFile').files[0];
    if (!f) return false;
    const bmp = await createImageBitmap(f, { imageOrientation: 'from-image' });
    const s = Math.max(240 / bmp.width, 320 / bmp.height);
    ctx.drawImage(bmp, (240 - bmp.width * s) / 2, (320 - bmp.height * s) / 2, bmp.width * s, bmp.height * s);
    if ($('bgDim').checked) { ctx.fillStyle = 'rgba(0,0,0,0.35)'; ctx.fillRect(0, 0, 240, 320); }
    return true;
  }
  $('bgFile').onchange = () => bgDraw().then(() => say('Đã xem trước hình nền bên dưới.'));
  $('bgDim').onchange = () => bgDraw();
  $('bgSet').onclick = async () => {
    if (!await bgDraw()) { say('Chưa chọn ảnh'); return; }
    say('Đang gửi hình nền...');
    send('/clockbg', 'bg.bin').then(t => { say(t); bgInfo(); }).catch(e => say('Lỗi: ' + e, true));
  };
  $('bgClear').onclick = () => fetch('/clockbg/clear', { method: 'POST' }).then(r => r.text()).then(t => { say(t); bgInfo(); });
  bgInfo();
</script>
</body>
</html>
)rawliteral";
