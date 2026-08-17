#pragma once

// Config web UI — served from the board's access point (first 10 min after
// power-on, or until the phone disconnects). Single page, no dependencies.
// All values on the wire (dB mapped from -80..-20) so the meter needs no scaling.
static const char UI_PAGE[] PROGMEM = R"HTML(<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Bark Cam</title>
<style>
body{background:#111;color:#eee;font-family:system-ui,sans-serif;max-width:480px;margin:0 auto;padding:16px}
h1{font-size:20px;margin:8px 0;text-align:center}
h3{font-size:12px;color:#8f8;margin:18px 0 4px;text-transform:uppercase;letter-spacing:.08em}
.row{margin:8px 0;display:flex;align-items:center;gap:8px}
input,select{background:#222;color:#eee;border:1px solid #444;border-radius:6px;padding:8px;flex:1;font-size:15px;min-width:0}
button{background:#2d7;color:#111;text-align:center;border:0;border-radius:6px;padding:12px;font-size:15px;margin:4px 4px 4px 0;font-weight:600}
button:active{opacity:.6} #msg{min-height:20px;color:#8f8;font-size:14px;text-align:center}
canvas{background:#000;border-radius:6px;width:100%;height:90px}
label{font-size:13px;color:#aaa;white-space:nowrap;width:86px}
.hint{font-size:12px;color:#777} .center{text-align:center}
.help{flex:none;width:20px;height:20px;border-radius:50%;background:#345;color:#8cf;display:flex;align-items:center;justify-content:center;font-size:12px;font-weight:700}
a{color:#4cf}
</style></head><body>
<h1>&#128021; Bark Cam</h1>
<canvas id="m" width="400" height="90"></canvas>
<div class="hint center">live mic level — orange line is the bark threshold</div>

<h3>Mic settings</h3>
<div class="row"><label>Sensitivity</label><input type="range" id="sens" min="5" max="40" step="1"><span id="sensv" class="hint"></span></div>

<h3>Camera settings</h3>
<div class="row"><label>Rotation</label><select id="rot">
<option value="0">none</option><option value="3" selected>180&deg;</option></select></div>
<div class="row"><label>Exposure</label><select id="expo">
<option value="0">dim</option><option value="1" selected>medium</option>
<option value="2">bright</option></select></div>

<h3>Wi-Fi</h3>
<div class="row"><label>SSID</label><input id="ssid"></div>
<div class="row"><label>Password</label><input id="pass" type="password"></div>

<h3>Telegram</h3>
<div class="row"><label>Bot token</label><input id="token"><span class="help" onclick="toggleHelp('htok')">?</span></div>
<div class="hint" id="htok" style="display:none">Get it from @BotFather on Telegram: message BotFather, send /token &lt;yourbotname&gt; (or /newbot to create one) — copy the token it replies with.</div>
<div class="row"><label>User ID</label><input id="chatid"><span class="help" onclick="toggleHelp('huid')">?</span></div>
<div class="hint" id="huid" style="display:none">Message @userinfobot on Telegram — it replies with your numeric user ID. Paste that number here.</div>

<div class="center" style="margin-top:16px">
<button onclick="save()">Save settings</button><button onclick="testSend()">Test send (photo)</button>
</div>
<div class="hint center">Disconnect to save and exit.</div>
<div id="msg"></div>
<p class="hint center" style="margin-top:14px"><a href="https://buymeacoffee.com/snail3d" target="_blank">&#9749; Buy the designer a coffee</a></p>
<script>
const $=id=>document.getElementById(id);
let savedToken='';
function load(){fetch('/config').then(r=>r.json()).then(c=>{
  $('ssid').value=c.ssid;$('pass').value=c.pass;
  savedToken=c.token||'';$('token').value=savedToken?savedToken.slice(0,12)+'\u2026':'';
  $('chatid').value=c.chatId;
  $('sens').value=c.margin;sensLabel();$('rot').value=String(c.rotate);
  $('expo').value=String(c.exposure);
}).catch(()=>{});}
function sensLabel(){$('sensv').textContent=$('sens').value+' dB (lower = more sensitive)';}
$('sens').oninput=sensLabel;
function toggleHelp(id){const e=document.getElementById(id);e.style.display=e.style.display==='none'?'block':'none';}
function draw(l){const c=$('m'),x=c.getContext('2d');
  x.fillStyle='#000';x.fillRect(0,0,c.width,c.height);
  const n=l.hist.length,bw=c.width/n;
  for(let i=0;i<n;i++){const v=Math.max(0,Math.min(1,l.hist[i]));
    x.fillStyle=v>l.thr?'#f55':'#2d7';
    const h=v*c.height*0.95;x.fillRect(i*bw,c.height-h,bw-1,h);}
  const ty=c.height*(1-Math.max(0,Math.min(1,l.thr)));
  x.fillStyle='#fa0';x.fillRect(0,ty,c.width,1);}
setInterval(async()=>{try{const l=await(await fetch('/level')).json();draw(l);}catch(e){}},300);
function save(){
  if(!confirm('This will override any current settings. Continue?'))return;
  const p=new URLSearchParams();
  ['ssid','pass','chatid'].forEach(k=>p.set(k==='chatid'?'chatId':k,$(k).value));
  if($('token').value!==(savedToken?savedToken.slice(0,12)+'\u2026':'')) p.set('token',$('token').value);
  p.set('margin',$('sens').value);
  p.set('rotate',$('rot').value);p.set('exposure',$('expo').value);
  fetch('/config',{method:'POST',body:p}).then(r=>r.json())
    .then(j=>$('msg').textContent=j.ok?'Saved \u2713 — you can disconnect now; the board closes its access point when your phone leaves.':'Save failed');}
function testSend(){$('msg').textContent='Sending test photo\u2026';
  fetch('/test',{method:'POST'}).then(r=>r.json()).catch(()=>{});
  let n=0;const t=setInterval(async()=>{try{
    const s=await(await fetch('/status')).json();
    if(s.lastResult==='ok'){$('msg').textContent='Test photo sent \u2713 check Telegram';clearInterval(t);}
    else if(s.lastResult==='fail'){$('msg').textContent='Test failed \u2014 check serial log';clearInterval(t);}
    else if(++n>90){$('msg').textContent='Still working\u2026 (check serial)';clearInterval(t);}}catch(e){}},1000);}
load();
</script></body></html>)HTML";
