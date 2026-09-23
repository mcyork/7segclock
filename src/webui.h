// The settings page served once the clock is on your network.
//
// Everything is inline in one PROGMEM string: no filesystem, no separate CSS or
// JS fetch, so the page cannot half-load and there is nothing to keep in sync
// with the firmware. Pure ASCII, declared UTF-8.
//
// Controls post to /set and the page repaints from the reply - no reload, no
// "Save" round trip for a colour you are still dragging. Since 1.2.0 every write
// is a POST with the X-7seg header (see requireWrite() in main.cpp), and sends
// are coalesced: one request in flight, the latest values queued behind it, so a
// slider drag is a handful of requests instead of one per pixel and the reply
// that repaints the slider is never older than what you dragged to.
#pragma once
#include <Arduino.h>

const char SETTINGS_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang=en>
<meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>mini7seg clock</title>
<style>
:root{--bg:#0d0f13;--fg:#e8eaed;--mut:#8b929c;--line:#252a32;--acc:#00a0ff;--bad:#ff6b6b}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,-apple-system,sans-serif}
.w{max-width:26rem;margin:0 auto;padding:1.5rem 1.1rem 3rem}
h1{font-size:1.1rem;margin:0 0 .2rem;letter-spacing:.01em}
.sub{color:var(--mut);font-size:.82rem;margin:0 0 .4rem}
.err{color:var(--bad);font-size:.82rem;min-height:1.2em;margin:0 0 1.2rem}
fieldset{border:0;padding:0;margin:0 0 1.5rem}
legend{font-size:.72rem;text-transform:uppercase;letter-spacing:.09em;color:var(--mut);padding:0 0 .5rem}
.modes{display:grid;grid-template-columns:repeat(2,1fr);gap:.5rem}
.modes button{padding:.7rem .5rem;border:1px solid var(--line);background:#151920;color:var(--fg);
  border-radius:.5rem;font:inherit;font-size:.9rem;cursor:pointer;transition:.12s}
.modes button:hover{border-color:#39414d}
.modes button[aria-pressed=true]{background:var(--acc);border-color:var(--acc);color:#04121d;font-weight:600}
.row{display:flex;align-items:center;gap:.8rem;padding:.55rem 0}
.row label{flex:1;font-size:.92rem}
.row output{color:var(--mut);font-variant-numeric:tabular-nums;font-size:.85rem;min-width:2.6rem;text-align:right}
input[type=range]{flex:2;accent-color:var(--acc)}
input[type=color]{width:3.2rem;height:2.1rem;padding:0;border:1px solid var(--line);border-radius:.4rem;background:none}
input[type=text],input[type=password],select{flex:2;background:#151920;color:var(--fg);border:1px solid var(--line);
  border-radius:.4rem;padding:.45rem .6rem;font:inherit;font-size:.9rem;min-width:0}
.seg{display:flex;gap:.3rem;flex-wrap:wrap}
.seg button{flex:1;padding:.5rem;border:1px solid var(--line);background:#151920;color:var(--fg);
  border-radius:.4rem;font:inherit;font-size:.82rem;cursor:pointer}
.seg button[aria-pressed=true]{background:#1f2630;border-color:var(--acc);color:var(--acc)}
.seg button.warn{border-color:#5a2a2a;color:var(--bad)}
.act{display:block;width:100%;margin-top:.6rem;padding:.5rem;border:1px solid var(--acc);background:#151920;
  color:var(--acc);border-radius:.4rem;font:inherit;cursor:pointer}
.note{color:var(--mut);font-size:.78rem;border-top:1px solid var(--line);padding-top:1rem;margin-top:.5rem}
.note b{color:var(--fg);font-weight:600}
.flat{border:0;padding:0;margin:.4rem 0 0}
</style>
<div class=w>
<h1 id=title>mini7seg clock</h1>
<p class=sub id=stat>&nbsp;</p>
<p class=err id=err></p>

<fieldset><legend>Hue source</legend>
<div class=modes id=hue>
<button data-v=0>Fixed</button><button data-v=1>Cycle</button><button data-v=2>Chrono</button>
</div></fieldset>

<fieldset><legend>Colour</legend>
<div class=row><label for=col>Colour</label><input type=color id=col value="#00a0ff"></div>
<div class=row><label for=spr>Spread across digits</label><input type=range id=spr min=0 max=64><output id=sprv></output></div>
<div class=row><label for=bri>Brightness</label><input type=range id=bri min=5 max=255><output id=briv></output></div>
<div class=row><label for=spd>Animation speed</label><input type=range id=spd min=1 max=20><output id=spdv></output></div>
<div class=row><label id=envl>Breathe</label><div class=seg id=env aria-labelledby=envl><button data-v=0>Off</button><button data-v=1>On</button></div></div>
</fieldset>

<fieldset><legend>Seconds &mdash; path</legend>
<div class=modes id=secpath>
<button data-v=0>Off</button><button data-v=1>Trace</button>
<button data-v=2>Ghost</button><button data-v=3>Orbit</button>
<button data-v=4>Ring</button>
</div></fieldset>

<fieldset><legend>Seconds &mdash; trail</legend>
<div class=modes id=sectrail>
<button data-v=0>None</button><button data-v=1>Comet</button><button data-v=2>Fill</button>
</div></fieldset>

<fieldset><legend>Clock</legend>
<div class=row><label id=h12l>Hour format</label><div class=seg id=h12 aria-labelledby=h12l><button data-v=0>24h</button><button data-v=1>12h</button></div></div>
<div class=row><label id=coll>Colon</label><div class=seg id=colon aria-labelledby=coll><button data-v=0>Blink</button><button data-v=1>On</button><button data-v=2>Off</button></div></div>
<div class=row><label for=tzsel>Timezone</label><select id=tzsel></select></div>
<div class=row id=tzrow><label for=tz>POSIX rule</label><input type=text id=tz maxlength=47 spellcheck=false autocapitalize=off></div>
<p class="note flat">The clock applies the rule itself, so daylight saving rolls on its own. Pick a zone, or enter any POSIX TZ string.</p>
</fieldset>

<fieldset><legend>Ticker</legend>
<div class=row><label for=tick>Show every</label><input type=range id=tick min=0 max=60><output id=tickv></output></div>
<div class=row><label id=cardl>Cards</label><div class=seg id=cards aria-labelledby=cardl>
<button data-v=1>Bitcoin</button><button data-v=2>Temp</button></div></div>
<p class="note flat" id=btcn></p>
</fieldset>

<fieldset><legend>Hardware &mdash; LED data pin</legend>
<div class=modes id=pin></div>
<p class="note flat" id=pinn></p>
<div id=pinr></div>
</fieldset>

<fieldset><legend>Device</legend>
<div class=row><label for=name>Name</label><input type=text id=name maxlength=24 spellcheck=false autocapitalize=off pattern="[a-z0-9-]+"></div>
<p class="note flat" id=namen></p>
<div id=namer></div>
</fieldset>

<fieldset><legend>Network</legend>
<div class=row><label for=ssid>WiFi network</label><input type=text id=ssid maxlength=32 spellcheck=false autocapitalize=off autocorrect=off></div>
<div class=row><label for=pass>Password</label><input type=password id=pass maxlength=63 placeholder="blank for an open network"></div>
<div class=seg><button id=netsave>Save network and restart</button></div>
<p class="note flat" id=netn>Changing networks restarts the clock. If it cannot join, it waits ten minutes and then raises its own <b>setup network</b> so you can try again.</p>
<div class=seg style="margin-top:.8rem"><button id=freset class=warn>Factory reset</button></div>
</fieldset>

<fieldset><legend>Firmware</legend>
<div class=row><label>Version</label><output id=fwv></output></div>
<div class=seg><button id=chk>Check for updates</button></div>
<p class="note flat" id=fwn></p>
<p class="note flat" id=upderr></p>
</fieldset>

<p class=note id=note></p>
<p class=note style="border:0"><a href="/wiring" style="color:var(--acc)">Display wiring and shape &rarr;</a></p>
<p class=note style="border:0"><a href="/update" style="color:var(--acc)">Upload a .bin manually &rarr;</a></p>
<p class=note style="border:0">Anyone on this network can use this page; there is no password. Run the clock on a network you trust.</p>
</div>
<script>
let S={};
const $=i=>document.getElementById(i);
const H={method:'POST',headers:{'X-7seg':'1'}};
// Coalesced sends: one request in flight, the newest values behind it.
let inflight=false,pending=null;
function send(o){pending=Object.assign(pending||{},o);Object.assign(S,o);if(!inflight)flush()}
function flush(){if(!pending)return;const o=pending;pending=null;inflight=true;
  fetch('/set?'+new URLSearchParams(o),H).then(r=>r.json()).then(d=>{inflight=false;
    if(d.ok===false){$('err').textContent='Not applied: '+(d.error||'unknown');reload();}
    // A reply that predates queued values would snap the control back; only the
    // last reply in a burst repaints.
    else{$('err').textContent='';if(!pending)paint(d);}
    flush()}).catch(()=>{inflight=false;$('err').textContent='The clock did not answer.';flush()})}
const reload=()=>fetch('/api').then(r=>r.json()).then(paint).catch(()=>{$('err').textContent='The clock did not answer.'});
const TZ=[['US Pacific','PST8PDT,M3.2.0,M11.1.0'],['US Mountain','MST7MDT,M3.2.0,M11.1.0'],['US Arizona','MST7'],
 ['US Central','CST6CDT,M3.2.0,M11.1.0'],['US Eastern','EST5EDT,M3.2.0,M11.1.0'],['Canada Atlantic','AST4ADT,M3.2.0,M11.1.0'],
 ['Newfoundland','NST3:30NDT,M3.2.0,M11.1.0'],['UK / Ireland','GMT0BST,M3.5.0/1,M10.5.0'],['Central Europe','CET-1CEST,M3.5.0,M10.5.0/3'],
 ['Eastern Europe','EET-2EEST,M3.5.0/3,M10.5.0/4'],['India','IST-5:30'],['China','CST-8'],['Japan','JST-9'],
 ['Australia East','AEST-10AEDT,M10.1.0,M4.1.0/3'],['New Zealand','NZST-12NZDT,M9.5.0,M4.1.0/3'],['UTC','UTC0']];
// The compiled-in default spelled the switch hour explicitly; treat it as US Pacific.
const tzAlias=v=>v==='PST8PDT,M3.2.0/2,M11.1.0/2'?'PST8PDT,M3.2.0,M11.1.0':v;
$('tzsel').innerHTML=TZ.map(t=>'<option value="'+t[1]+'">'+t[0]+'</option>').join('')+'<option value="">Custom...</option>';
function stat(d){const s=[d.time,d.ip||'no ip',d.online?'online':'offline'];
  if(d.syncMin!=null)s.push('synced '+(d.syncMin<1?'just now':d.syncMin+' min ago'));else if(!d.timeValid)s.push('waiting for NTP');
  $('stat').textContent=s.join('  -  ')}
function paint(d){S=d;
  [...$('hue').children].forEach(b=>b.setAttribute('aria-pressed',+b.dataset.v===d.hue));
  [...$('env').children].forEach(b=>b.setAttribute('aria-pressed',+b.dataset.v===d.env));
  [...$('secpath').children].forEach(b=>b.setAttribute('aria-pressed',+b.dataset.v===d.secpath));
  [...$('sectrail').children].forEach(b=>b.setAttribute('aria-pressed',+b.dataset.v===d.sectrail));
  [...$('h12').children].forEach(b=>b.setAttribute('aria-pressed',+b.dataset.v===(d.h12?1:0)));
  [...$('colon').children].forEach(b=>b.setAttribute('aria-pressed',+b.dataset.v===d.colon));
  $('col').value='#'+[d.r,d.g,d.b].map(x=>x.toString(16).padStart(2,'0')).join('');
  $('spr').value=d.spread; $('sprv').textContent=d.spread?d.spread:'none';
  $('bri').value=d.bri; $('briv').textContent=d.bri;
  $('spd').value=d.speed; $('spdv').textContent=d.speed;
  $('tick').value=d.tick; $('tickv').textContent=d.tick?d.tick+' min':'off';
  // Cards is a bitmask, so each button reflects its own bit rather than equality.
  [...$('cards').children].forEach(b=>b.setAttribute('aria-pressed',!!(d.cards & +b.dataset.v)));
  $('btcn').textContent=!d.tick?'':
    [d.btc?('BTC $'+d.btc.toLocaleString()):'BTC not fetched',
     d.tempF!=null?(d.tempF.toFixed(1)+'\u00b0F'+(d.city?' in '+d.city:'')):'temp not fetched'].join('  \u00b7  ');
  stat(d);
  $('fwv').textContent=d.fw;
  $('title').textContent=(d.name||'mini7seg')+' clock';
  // Timezone: the select shows a known zone or "Custom"; the text field always holds the rule.
  const tz=tzAlias(d.tz||'');const known=TZ.some(t=>t[1]===tz);
  if(document.activeElement!==$('tz')){$('tz').value=d.tz||'';$('tzsel').value=known?tz:'';}   // not while typing a rule
  // Built once from the value the firmware reports, not hard-coded here: the
  // allow-list lives in settings.h next to the switch that implements it, and
  // two copies of it would eventually disagree.
  if(!$('pin').children.length)
    $('pin').innerHTML=(d.pins||[]).map(p=>'<button data-v='+p+'>GPIO '+p+'</button>').join('');
  [...$('pin').children].forEach(b=>b.setAttribute('aria-pressed',+b.dataset.v===d.pin));
  $('pinn').innerHTML='Currently driving <b>GPIO '+d.bootPin+'</b>. Only pins this chip can safely drive are listed '+
    '\u2014 strapping pins are withheld, because they are read at reset to decide how the chip boots and '+
    'a WS2812 line idles low, which can hold the board in download mode and look exactly like a dead clock.';
  // The restart offer is part of the painted state (pin != bootPin), not a one-off
  // element appended after a click: that element was destroyed by the very repaint
  // its own request triggered.
  restartOffer('pinr',d.pin!==d.bootPin,'Restart now to use GPIO '+d.pin);
  if(document.activeElement!==$('name'))$('name').value=d.name||'';
  $('namen').innerHTML='Reachable as <b>'+(d.host||d.name)+'.local</b>. The name is also the hostname your router shows and the stem of the setup network. Takes effect on restart.';
  restartOffer('namer',d.name!==d.host,'Restart now to become '+d.name+'.local');
  // Colour and speed only do anything in the modes that read them. Saying so
  // beats greying controls out and leaving people guessing why.
  const hue={0:'<b>Fixed</b> - the colour you pick.',
             1:'<b>Cycle</b> - hue drifts through the spectrum at the speed set above.',
             2:'<b>Chrono</b> - hue <i>is</i> the time of day: cold overnight, warm at noon, violet by evening. After a few days you read the hour off the colour before the digits.'}[d.hue];
  const spr=d.spread?' Digits are spread '+d.spread+' apart on the wheel, so it reads as a gradient.':'';
  const env=d.env?' Brightness swells gently.':'';
  const sec={0:'',
             1:' Cursor walks the lit segments, tracing the numerals clockwise.',
             2:' Cursor walks every segment, lit or not - the beat never depends on the time.',
             3:' One cursor per digit, each round its own ring; laps every 6 s.',
             4:' One cursor round the outside of the whole display; on four digits it laps once a MINUTE, so its position is a real second hand.'}[d.secpath];
  const tr={0:'',1:' Short fading tail.',2:' Everything passed stays lit, so it reads as a filling dial.'}[d.sectrail];
  $('note').innerHTML=hue+spr+env+sec+tr;
  // Its own line, so a past failure never overwrites a fresh update check.
  $('upderr').textContent=d.updErr?'Last update attempt failed: '+d.updErr:'';
}
function restartOffer(id,show,label){const n=$(id);n.innerHTML='';if(!show)return;
  const b=document.createElement('button');b.className='act';b.textContent=label;
  b.onclick=()=>{b.textContent='Restarting\u2026';fetch('/reboot',H).catch(()=>{});setTimeout(()=>location.reload(),5000)};
  n.appendChild(b)}
$('hue').onclick=e=>{if(e.target.dataset.v!==undefined)send({hue:+e.target.dataset.v})};
$('env').onclick=e=>{if(e.target.dataset.v!==undefined)send({env:+e.target.dataset.v})};
$('secpath').onclick=e=>{if(e.target.dataset.v!==undefined)send({secpath:+e.target.dataset.v})};
$('sectrail').onclick=e=>{if(e.target.dataset.v!==undefined)send({sectrail:+e.target.dataset.v})};
$('spr').oninput=e=>{$('sprv').textContent=e.target.value>0?e.target.value:'none';send({spread:+e.target.value})};
$('h12').onclick=e=>{if(e.target.dataset.v!==undefined)send({h12:+e.target.dataset.v})};
$('colon').onclick=e=>{if(e.target.dataset.v!==undefined)send({colon:+e.target.dataset.v})};
$('col').oninput=e=>{const v=e.target.value;
  send({r:parseInt(v.substr(1,2),16),g:parseInt(v.substr(3,2),16),b:parseInt(v.substr(5,2),16)})};
$('bri').oninput=e=>{$('briv').textContent=e.target.value;send({bri:+e.target.value})};
$('spd').oninput=e=>{$('spdv').textContent=e.target.value;send({speed:+e.target.value})};
$('tick').oninput=e=>{$('tickv').textContent=e.target.value>0?e.target.value+' min':'off';send({tick:+e.target.value})};
$('cards').onclick=e=>{const v=+e.target.dataset.v;if(v)send({cards:S.cards^v})};
$('pin').onclick=e=>{const v=e.target.dataset.v;if(v!==undefined)send({pin:+v})};
$('tzsel').onchange=e=>{if(e.target.value){$('tz').value=e.target.value;send({tz:e.target.value})}else $('tz').focus()};
$('tz').onchange=e=>{const v=e.target.value.trim();if(v)send({tz:v})};
$('name').onchange=e=>{const v=e.target.value.trim().toLowerCase();if(v)send({name:v})};
$('netsave').onclick=()=>{const ssid=$('ssid').value.trim(),pass=$('pass').value;
  if(!ssid){$('err').textContent='Enter the network name first.';return}
  if(!confirm('Join "'+ssid+'" and restart the clock?'))return;
  $('netn').textContent='Saving and restarting...';
  fetch('/save?'+new URLSearchParams({ssid,pass}),H).then(r=>r.json()).then(j=>{
    $('netn').textContent=j.ok?'Saved. Restarting - the clock joins '+ssid+' and comes back at '+(S.host||'mini7seg')+'.local.':'Not saved: '+(j.error||'unknown')})
  .catch(()=>{$('netn').textContent='Restarting...'})};
$('freset').onclick=()=>{if(!confirm('Forget the WiFi network, the wiring map and every setting, then restart?'))return;
  $('netn').textContent='Resetting...';
  fetch('/factoryreset',H).then(r=>r.json()).then(j=>{$('netn').textContent=j.ok?'Reset. The clock restarts and raises its setup network.':'Not reset: '+(j.error||'unknown')})
  .catch(()=>{$('netn').textContent='Reset sent; the clock is restarting.'})};
$('chk').onclick=()=>{const n=$('fwn');n.textContent='Checking...';
  fetch('/checkupdate',H).then(r=>r.json()).then(u=>{
    if(!u.ok){n.textContent=u.code==404?'No release has been published yet.':
      u.code==403?'GitHub rate limit reached (60 checks an hour per address). Try again later.':
      u.code?'GitHub answered HTTP '+u.code+'.':'Could not reach GitHub.';return}
    if(!u.newer){n.textContent='Up to date ('+u.current+'; latest release '+u.latest+').';return}
    n.textContent='Version '+u.latest+' available. ';
    const b=document.createElement('button');b.textContent='Install now';
    b.style.cssText='width:auto;padding:.4rem .8rem;margin-left:.4rem';
    b.onclick=()=>{n.textContent='Downloading and installing '+u.latest+' - about 30 s. The clock restarts on its own; do not power it off.';
      fetch('/doupdate',H).then(r=>r.json()).then(j=>{if(j.ok===false)n.textContent='Refused: '+(j.error||'unknown');else watchUpdate(u.latest)})
      .catch(()=>watchUpdate(u.latest))};
    n.appendChild(b)}).catch(()=>{n.textContent='Could not reach the clock.'})};
// After Install: poll until the new version answers, or the firmware reports why it did not.
function watchUpdate(target){let tries=0;const t=setInterval(()=>{tries++;
  fetch('/api').then(r=>r.json()).then(d=>{
    if(d.fw===target){clearInterval(t);$('fwn').textContent='Now running '+d.fw+'.';paint(d);return}
    if(d.updErr){clearInterval(t);$('fwn').textContent='Update failed: '+d.updErr}
  }).catch(()=>{});
  if(tries>30){clearInterval(t);$('fwn').textContent='Still waiting for the clock to come back; reload this page in a minute.'}},3000)}
reload();
setInterval(()=>fetch('/api').then(r=>r.json()).then(stat).catch(()=>{$('stat').textContent='clock not answering'}),10000);
</script>
</html>
)HTML";
