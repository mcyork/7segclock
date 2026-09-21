// The settings page served once the clock is on your network.
//
// Everything is inline in one PROGMEM string: no filesystem, no separate CSS or
// JS fetch, so the page cannot half-load and there is nothing to keep in sync
// with the firmware. It is about 3 KB.
//
// Controls post to /set as a query string and the page updates live — no reload,
// no "Save" round trip for a colour you are still dragging.
#pragma once
#include <Arduino.h>

const char SETTINGS_HTML[] PROGMEM = R"HTML(<!doctype html>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>mini7seg clock</title>
<style>
:root{--bg:#0d0f13;--fg:#e8eaed;--mut:#8b929c;--line:#252a32;--acc:#00a0ff}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,-apple-system,sans-serif}
.w{max-width:26rem;margin:0 auto;padding:1.5rem 1.1rem 3rem}
h1{font-size:1.1rem;margin:0 0 .2rem;letter-spacing:.01em}
.sub{color:var(--mut);font-size:.82rem;margin:0 0 1.6rem}
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
.seg{display:flex;gap:.3rem}
.seg button{flex:1;padding:.5rem;border:1px solid var(--line);background:#151920;color:var(--fg);
  border-radius:.4rem;font:inherit;font-size:.82rem;cursor:pointer}
.seg button[aria-pressed=true]{background:#1f2630;border-color:var(--acc);color:var(--acc)}
.note{color:var(--mut);font-size:.78rem;border-top:1px solid var(--line);padding-top:1rem;margin-top:.5rem}
.note b{color:var(--fg);font-weight:600}
</style>
<div class=w>
<h1>mini7seg clock</h1>
<p class=sub id=stat>&nbsp;</p>

<fieldset><legend>Hue source</legend>
<div class=modes id=hue>
<button data-v=0>Fixed</button><button data-v=1>Cycle</button><button data-v=2>Chrono</button>
</div></fieldset>

<fieldset><legend>Colour</legend>
<div class=row><label for=col>Colour</label><input type=color id=col value="#00a0ff"></div>
<div class=row><label for=spr>Spread across digits</label><input type=range id=spr min=0 max=64><output id=sprv></output></div>
<div class=row><label for=bri>Brightness</label><input type=range id=bri min=5 max=255><output id=briv></output></div>
<div class=row><label for=spd>Animation speed</label><input type=range id=spd min=1 max=20><output id=spdv></output></div>
<div class=row><label>Breathe</label><div class=seg id=env><button data-v=0>Off</button><button data-v=1>On</button></div></div>
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
<div class=row><label>Hour format</label><div class=seg id=h12><button data-v=0>24h</button><button data-v=1>12h</button></div></div>
<div class=row><label>Colon</label><div class=seg id=colon><button data-v=0>Blink</button><button data-v=1>On</button><button data-v=2>Off</button></div></div>
</fieldset>

<fieldset><legend>Ticker</legend>
<div class=row><label for=tick>Show every</label><input type=range id=tick min=0 max=60><output id=tickv></output></div>
<div class=row><label>Cards</label><div class=seg id=cards>
<button data-v=1>Bitcoin</button><button data-v=2>Temp</button></div></div>
<p class=note id=btcn style="border:0;padding:0;margin:.2rem 0 0"></p>
</fieldset>

<fieldset><legend>Firmware</legend>
<div class=row><label>Version</label><output id=fwv></output></div>
<div class=seg><button id=chk>Check for updates</button></div>
<p class=note id=fwn style="border:0;padding:0;margin:.4rem 0 0"></p>
</fieldset>

<p class=note id=note></p>
<p class=note style="border:0"><a href="/update" style="color:var(--acc)">Upload a .bin manually &rarr;</a></p>
</div>
<script>
let S={};
const $=i=>document.getElementById(i);
const send=o=>{Object.assign(S,o);
  fetch('/set?'+new URLSearchParams(o)).then(r=>r.json()).then(paint);};
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
     d.tempF?(d.tempF.toFixed(1)+'\u00b0F'+(d.city?' in '+d.city:'')):'temp not fetched'].join('  \u00b7  ');
  $('stat').textContent=d.time+'  ·  '+d.ip;
  $('fwv').textContent=d.fw;
  // Colour and speed only do anything in the modes that read them. Saying so
  // beats greying controls out and leaving people guessing why.
  const hue={0:'<b>Fixed</b> — the colour you pick.',
             1:'<b>Cycle</b> — hue drifts through the spectrum at the speed set above.',
             2:'<b>Chrono</b> — hue <i>is</i> the time of day: cold overnight, warm at noon, violet by evening. After a few days you read the hour off the colour before the digits.'}[d.hue];
  const spr=d.spread?' Digits are spread '+d.spread+' apart on the wheel, so it reads as a gradient.':'';
  const env=d.env?' Brightness swells gently.':'';
  const sec={0:'',
             1:' Cursor walks the lit segments, tracing the numerals clockwise.',
             2:' Cursor walks all 28 segments, lit or not — the beat never depends on the time.',
             3:' Four cursors, one per digit, each round its own ring; laps every 6 s.',
             4:' One cursor round the outside of the whole display. Twelve segments, 60/12=5, so it laps once a MINUTE — its position is a real second hand.'}[d.secpath];
  const tr={0:'',1:' Short fading tail.',2:' Everything passed stays lit, so it reads as a filling dial.'}[d.sectrail];
  $('note').innerHTML=hue+spr+env+sec+tr;
}
$('hue').onclick=e=>{if(e.target.dataset.v)send({hue:+e.target.dataset.v})};
$('env').onclick=e=>{if(e.target.dataset.v!==undefined)send({env:+e.target.dataset.v})};
$('secpath').onclick=e=>{if(e.target.dataset.v!==undefined)send({secpath:+e.target.dataset.v})};
$('sectrail').onclick=e=>{if(e.target.dataset.v!==undefined)send({sectrail:+e.target.dataset.v})};
$('spr').oninput=e=>{$('sprv').textContent=e.target.value>0?e.target.value:'none';send({spread:+e.target.value})};
$('h12').onclick=e=>{if(e.target.dataset.v)send({h12:+e.target.dataset.v})};
$('colon').onclick=e=>{if(e.target.dataset.v!==undefined)send({colon:+e.target.dataset.v})};
$('col').oninput=e=>{const v=e.target.value;
  send({r:parseInt(v.substr(1,2),16),g:parseInt(v.substr(3,2),16),b:parseInt(v.substr(5,2),16)})};
$('bri').oninput=e=>{$('briv').textContent=e.target.value;send({bri:+e.target.value})};
$('spd').oninput=e=>{$('spdv').textContent=e.target.value;send({speed:+e.target.value})};
$('tick').oninput=e=>{$('tickv').textContent=e.target.value>0?e.target.value+' min':'off';send({tick:+e.target.value})};
$('cards').onclick=e=>{const v=+e.target.dataset.v;if(v)send({cards:S.cards^v})};
$('chk').onclick=()=>{const n=$('fwn');n.textContent='Checking…';
  fetch('/checkupdate').then(r=>r.json()).then(u=>{
    if(!u.ok){n.textContent='Could not reach GitHub.';return}
    if(!u.newer){n.textContent='Up to date ('+u.current+').';return}
    n.innerHTML='Version <b>'+u.latest+'</b> available. ';
    const b=document.createElement('button');b.textContent='Install now';
    b.style.cssText='width:auto;padding:.4rem .8rem;margin-left:.4rem';
    b.onclick=()=>{n.textContent='Downloading and installing — about 30 s. '+
      'The clock will restart on its own; do not power it off.';
      fetch('/doupdate').catch(()=>{});};
    n.appendChild(b);});};
fetch('/api').then(r=>r.json()).then(paint);
setInterval(()=>fetch('/api').then(r=>r.json()).then(d=>{
  $('stat').textContent=d.time+'  ·  '+d.ip;}),10000);
</script>
)HTML";
