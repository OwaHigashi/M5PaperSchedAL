"""Single-file dashboard (no external assets; LAN only)."""

DASHBOARD_HTML = r"""<!doctype html>
<html lang="ja"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>M5Paper Scheduler</title>
<style>
 body{font-family:system-ui,sans-serif;margin:0;background:#f4f4f4;color:#222}
 header{background:#222;color:#fff;padding:10px 16px;display:flex;gap:16px;align-items:center}
 header h1{font-size:18px;margin:0}
 main{padding:12px 16px;display:grid;grid-template-columns:1fr 1fr;gap:14px}
 @media(max-width:900px){main{grid-template-columns:1fr}}
 .card{background:#fff;border-radius:8px;padding:12px 14px;box-shadow:0 1px 3px rgba(0,0,0,.15)}
 .card h2{font-size:15px;margin:0 0 8px;border-bottom:1px solid #ddd;padding-bottom:4px}
 .kv{display:grid;grid-template-columns:max-content 1fr;gap:2px 12px;font-size:13px}
 .kv b{font-weight:600;color:#555}
 .on{color:#0a0;font-weight:700}.off{color:#c00;font-weight:700}
 table{border-collapse:collapse;width:100%;font-size:13px}
 td,th{padding:3px 6px;border-bottom:1px solid #eee;text-align:left;vertical-align:top}
 tr.past{color:#999} tr.next{background:#fff6d6}
 .al{white-space:nowrap} .al.tr{color:#999;text-decoration:line-through}
 button{margin:2px;padding:5px 10px;border:1px solid #888;background:#fafafa;border-radius:5px;cursor:pointer}
 button:hover{background:#e8e8e8}
 pre{font-size:12px;max-height:260px;overflow:auto;background:#fafafa;padding:6px;border:1px solid #eee}
 .wide{grid-column:1/-1}
 input[type=text]{padding:4px;width:260px}
 .anom{color:#b00}
</style></head><body>
<header><h1>M5Paper Scheduler</h1><span id="hdr"></span></header>
<main>
 <div class="card"><h2>端末 (M5Paper)</h2><div class="kv" id="dev"></div>
  <div style="margin-top:8px">
   <button onclick="cmd({cmd:'refresh'})">再同期</button>
   <button onclick="cmd({cmd:'redraw'})">再描画</button>
   <button onclick="cmd({cmd:'screenshot'})">スクリーンショット</button>
   <button onclick="cmd({cmd:'play',midi:'',duration:5,repeat:1})">サウンドテスト(5s)</button>
   <button onclick="cmd({cmd:'stop'})">停止</button>
   <button onclick="if(confirm('再起動しますか?'))cmd({cmd:'reboot'})">再起動</button>
   <br><input type="text" id="msg" placeholder="端末に表示するメッセージ"><button onclick="cmd({cmd:'message',text:document.getElementById('msg').value})">表示</button>
  </div>
  <h2 style="margin-top:10px">コマンド履歴</h2><pre id="cmds"></pre>
 </div>
 <div class="card"><h2>サーバ / ICS</h2><div class="kv" id="srv"></div>
  <div style="margin-top:8px"><button onclick="fetch('/api/v1/refresh',{method:'POST'}).then(load)">ICS再取得</button>
  <a href="/log/alarm" target="_blank"><button>アラームログ</button></a>
  <a href="/log/device" target="_blank"><button>端末ログ</button></a></div>
  <h2 style="margin-top:10px">メモリ監視 <span id="memlvl"></span></h2>
  <div class="kv" id="mem"></div>
  <svg id="memchart" width="100%" height="120" viewBox="0 0 600 120" preserveAspectRatio="none" style="background:#fafafa;border:1px solid #eee;margin-top:6px"></svg>
  <div style="font-size:11px;color:#666">黒: heap / 灰: maxBlock（直近24h, 5分刻み）<a href="/log/mem" target="_blank">CSV</a></div>
  <h2 style="margin-top:10px">異常検知</h2><pre id="anom"></pre>
  <h2 style="margin-top:10px">端末の操作イベント</h2><pre id="ui"></pre>
 </div>
 <div class="card wide"><h2>予定 (<span id="cnt"></span>)</h2>
  <table id="ev"><thead><tr><th>開始</th><th>予定</th><th>アラーム</th><th>MIDI</th></tr></thead><tbody></tbody></table>
 </div>
</main>
<script>
const $=s=>document.querySelector(s);
const f=(ts,withDate=true)=>{if(!ts)return '-';const d=new Date(ts*1000);const p=n=>String(n).padStart(2,'0');
 return (withDate?`${p(d.getMonth()+1)}/${p(d.getDate())} `:'')+`${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`};
const ago=ts=>ts?Math.round(Date.now()/1000-ts)+'s前':'-';
const esc=s=>String(s??'').replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));
function kv(el,obj){el.innerHTML=Object.entries(obj).map(([k,v])=>`<b>${esc(k)}</b><span>${v}</span>`).join('')}
async function cmd(c){const r=await fetch('/api/v1/cmd',{method:'POST',body:JSON.stringify(c)});load();}
async function load(){
 const s=await (await fetch('/api/v1/status')).json();
 const d=s.device, i=d.info||{};
 $('#hdr').textContent=`rev ${s.rev} · ${f(s.now)}`;
 kv($('#dev'),{
  状態:d.online?'<span class=on>ONLINE</span>':'<span class=off>OFFLINE</span>',
  最終受信:`${f(d.last_seen)} (${ago(d.last_seen)})`, IP:esc(d.addr), FW:esc(d.fw),
  稼働:`${i.uptime??'-'}s (再起動検知 ${d.reboots}回, reset=${esc(i.reset)})`,
  時刻ずれ:`${d.skew_sec}s`, 端末rev:`${d.rev} ${d.rev===s.rev?'✓':'⚠ (host '+s.rev+')'}`,
  画面:esc(i.ui), 鳴動中:esc(d.playing||'-'),
  heap:`${i.heap} / maxBlock ${i.maxblock} / psram ${i.psram}`,
  電池:`${i.bat} mV`, RSSI:`${i.rssi} dBm`, FS:`${i.fs_used}/${i.fs_total}`,
  同期失敗:`${i.sync_fail??0}`, ハートビート:`${d.heartbeats} (欠落 ${d.seq_gaps})`,
 });
 kv($('#srv'),{
  予定数:`${s.events} (切捨 ${s.trimmed})`, 取得範囲:`過去${s.window.past_days}日〜先${s.window.future_days}日`,
  最終取得:`${f(s.last_fetch)} (${ago(s.last_fetch)})`, 最終変更:f(s.last_change),
  次アラーム:f(s.next_alarm), ntfy:esc(s.config.ntfy_topic||'(off)'),
  ...Object.fromEntries(s.sources.map(x=>[`ICS${x.idx+1}`,`${x.ok===false?'<span class=off>NG</span>':'<span class=on>OK</span>'} ${x.bytes}B ${esc(x.error)} <small>${esc(x.url).slice(0,70)}</small>`]))
 });
 const m=s.memory||{}, st=m.stats||{};
 $('#memlvl').innerHTML=m.level==='ok'?'<span class=on>OK</span>':m.level==='unknown'?'-':`<span class=off>${m.level.toUpperCase()}</span>`;
 kv($('#mem'),{
  現在:`heap ${(st.heap/1024).toFixed(1)}KB / maxBlock ${(st.maxblock/1024).toFixed(1)}KB / psram ${(st.psram/1024).toFixed(0)}KB`,
  最小値:`heap ${(st.min_heap/1024).toFixed(1)}KB / maxBlock ${(st.min_maxblock/1024).toFixed(1)}KB (今回起動以降)`,
  傾き:`heap ${st.slope_heap_kb_h??'-'} KB/h, maxBlock ${st.slope_maxblock_kb_h??'-'} KB/h (${st.window_h}h窓, ${st.samples}点)`,
  起動後変化:st.since_boot?`heap ${(st.since_boot.heap/1024).toFixed(1)}KB / maxBlock ${(st.since_boot.maxblock/1024).toFixed(1)}KB (${st.since_boot.hours}h)`:'(起動5分後に基準化)',
  予測:st.hours_to_reboot_floor!=null?`このペースなら約 ${st.hours_to_reboot_floor} h で予防再起動しきい値`:'減少傾向なし',
  警告:(m.reasons||[]).join(' / ')||'なし',
 });
 fetch('/api/v1/memory').then(r=>r.json()).then(mm=>{
  const ser=mm.series||[]; const svg=$('#memchart'); if(ser.length<2){svg.innerHTML='';return;}
  const t0=ser[0][0], t1=ser[ser.length-1][0]||t0+1; const vals=ser.flatMap(p=>[p[1],p[2]]);
  const lo=Math.min(...vals)*0.95, hi=Math.max(...vals)*1.02;
  const X=t=>600*(t-t0)/Math.max(1,t1-t0), Y=v=>120-120*(v-lo)/Math.max(1,hi-lo);
  const path=(i,c)=>`<polyline fill="none" stroke="${c}" stroke-width="1.5" points="${ser.map(p=>X(p[0]).toFixed(1)+','+Y(p[i]).toFixed(1)).join(' ')}"/>`;
  svg.innerHTML=path(1,'#222')+path(2,'#999')+`<text x="4" y="12" font-size="11" fill="#444">${(hi/1024).toFixed(0)}KB</text><text x="4" y="116" font-size="11" fill="#444">${(lo/1024).toFixed(0)}KB</text>`;
 });
 $('#anom').textContent=d.anomalies.slice().reverse().map(a=>`${f(a.ts)} ${a.text}`).join('\n')||'(なし)';
 $('#ui').textContent=d.ui_events.slice().reverse().slice(0,30).map(e=>`${f(e.host_ts,false)} ${JSON.stringify(e)}`).join('\n')||'(なし)';
 $('#cmds').textContent=d.cmd_history.slice().reverse().map(h=>`#${h.cmd.id} ${h.cmd.cmd} ${h.cmd.src} ${h.ack?('→ '+(h.ack.ok?'OK':'NG')+' '+h.ack.info):'(未ack)'}`).join('\n')||'(なし)';
 const l=await (await fetch('/api/v1/list')).json();
 $('#cnt').textContent=l.events.length;
 const now=Date.now()/1000; let nextDone=false;
 $('#ev tbody').innerHTML=l.events.map(e=>{
  const past=e.start<now&&!e.allday; let cls=past?'past':'';
  if(!nextDone&&!past&&!e.allday){cls='next';nextDone=true;}
  const als=e.alarms.map(a=>`<span class="al ${a.state.triggered?'tr':''}" title="${esc(a.state.how)}">${f(a.at)} (${a.offset_min>0?a.offset_min+'分前':a.offset_min<0?(-a.offset_min)+'分後':'定刻'})</span>`).join('<br>');
  return `<tr class="${cls}"><td>${e.allday?f(e.start).slice(0,5)+' 終日':f(e.start)}</td><td>${esc(e.summary)}</td><td>${als}</td><td>${esc(e.midi_file)}${e.play_duration>=0?' @'+e.play_duration:''}${e.play_repeat>=0?' *'+e.play_repeat:''}</td></tr>`;
 }).join('');
}
load(); setInterval(load,5000);
</script></body></html>
"""
