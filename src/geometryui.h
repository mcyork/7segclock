// The wiring editor, served at /wiring.
//
// Why a second page rather than more controls on the settings page: this one is
// used ONCE, by the person who built the display, and never again. Putting a
// 64-cell table editor in front of everyone who only wants to change the colour
// would be the tail wagging the dog.
//
// The Learn wizard is the reason this exists. Every other way of configuring a
// hand-wired display -- a form of numbers, dragging LED indices onto segments --
// asks the builder for the one fact they do not have: the order they happened to
// solder in. This lights one LED group and asks what they see, which is a
// question anyone can answer while looking at the bench.
#pragma once
#include <Arduino.h>

const char GEOMETRY_HTML[] PROGMEM = R"HTML(<!doctype html>
<meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>mini7seg geometry</title>
<style>
:root{--bg:#0d0f13;--fg:#e8eaed;--mut:#8b929c;--line:#252a32;--acc:#00a0ff}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,-apple-system,sans-serif}
.w{max-width:26rem;margin:0 auto;padding:1.5rem 1.1rem 3rem}
h1{font-size:1.1rem;margin:0 0 .2rem}
.sub{color:var(--mut);font-size:.82rem;margin:0 0 1.6rem}
fieldset{border:0;padding:0;margin:0 0 1.5rem}
legend{font-size:.72rem;text-transform:uppercase;letter-spacing:.09em;color:var(--mut);padding:0 0 .5rem}
.modes{display:grid;grid-template-columns:repeat(2,1fr);gap:.5rem}
.modes button{padding:.7rem .5rem;border:1px solid var(--line);background:#151920;color:var(--fg);
 border-radius:.5rem;font:inherit;font-size:.9rem;cursor:pointer;transition:.12s}
.modes button[aria-pressed=true]{background:var(--acc);border-color:var(--acc);color:#04121d;font-weight:600}
.row{display:flex;align-items:center;gap:.8rem;padding:.55rem 0}
.row label{flex:1;font-size:.92rem}
.row output{color:var(--mut);font-variant-numeric:tabular-nums;font-size:.85rem;min-width:2.6rem;text-align:right}
input[type=range]{flex:2;accent-color:var(--acc)}
.seg{display:flex;gap:.3rem;flex-wrap:wrap}
.seg button{flex:1;min-width:2.2rem;padding:.5rem;border:1px solid var(--line);background:#151920;color:var(--fg);
 border-radius:.4rem;font:inherit;font-size:.82rem;cursor:pointer}
.seg button[aria-pressed=true]{background:#1f2630;border-color:var(--acc);color:var(--acc)}
.note{color:var(--mut);font-size:.78rem;border-top:1px solid var(--line);padding-top:1rem;margin-top:.5rem}
.note b{color:var(--fg);font-weight:600}
svg{width:100%;height:auto;display:block;background:#0a0c10;border:1px solid var(--line);border-radius:.5rem}
svg polygon,svg circle{fill:#171c24;stroke:#20262f;cursor:pointer;transition:fill .12s}
svg .on{fill:#1d3d52;stroke:#2c5f7f}
svg .hot{fill:var(--acc);stroke:var(--acc)}
svg text{fill:var(--mut);font-size:15px;pointer-events:none}
</style>
<div class=w>
<h1>display geometry</h1>
<p class=sub id=stat>Loading...</p>

<fieldset><legend>Shape</legend>
<div class=row><label for=nd>Digits</label><input type=range id=nd min=1 max=8><output id=ndv></output></div>
<div class=row><label for=lp>LEDs per segment</label><input type=range id=lp min=1 max=8><output id=lpv></output></div>
<div class=row><label>Decimal points</label><div class=seg id=dp></div></div>
</fieldset>

<fieldset><legend>Map</legend>
<svg id=svg></svg>
<p class=note id=hint style="border:0;padding:.7rem 0 0;margin:0"></p>
<div class=modes id=act style="margin-top:.6rem">
<button id=bid>Identify</button><button id=blr>Learn wiring</button></div>
<div class=seg id=nav style="margin-top:.5rem;display:none">
<button id=bback>Back</button><button id=bskip>Skip</button><button id=bstop>Cancel</button></div>
</fieldset>

<fieldset><legend>Reset</legend>
<div class=seg><button id=brst>Reset to standard wiring</button></div>
</fieldset>

<p class=note id=note>Learn mode lights one LED group at a time and asks which segment it was, so you never have to know your own wiring. The picture fills in as you go.</p>
</div>
<script>
const $=i=>document.getElementById(i);
const AB=65535;   // absent segment; must match SEGMENT_ABSENT in settings.h
const NM=['A top','B top right','C bottom right','D bottom','E bottom left','F top left','G middle','DP'];
let G={digits:4,ledsPerSeg:1,dpMask:0,segBase:[]},mode=0,step=0,tbl=null,busy=0,lastD=0,lastS=0;
const H=(x,y,w)=>[[x,y+7],[x+7,y],[x+w-7,y],[x+w,y+7],[x+w-7,y+14],[x+7,y+14]].join(' ');
const V=(x,y,h)=>[[x+7,y],[x+14,y+7],[x+14,y+h-7],[x+7,y+h],[x,y+h-7],[x,y+7]].join(' ');
const P=[H(6,6,72),V(64,22,49),V(64,89,49),H(6,140,72),V(6,89,49),V(6,22,49),H(6,73,72)];
const say=t=>$('stat').textContent=t;
const tab=()=>mode==2?tbl:G.segBase;
const has=(d,s)=>{const t=tab()[d];return t&&t[s]!=AB?'on':''};
function draw(){
  let s='',d,i;
  for(d=0;d<G.digits;d++){
    s+='<g transform="translate('+d*100+',0)">';
    for(i=0;i<7;i++)s+='<polygon class="'+has(d,i)+'" id="p'+d+'_'+i+'" points="'+P[i]+'"/>';
    if(G.dpMask&(1<<d))s+='<circle class="'+has(d,7)+'" id="p'+d+'_7" cx=90 cy=147 r=7/>';
    s+='<text x=40 y=172 text-anchor=middle>'+d+'</text></g>';
  }
  $('svg').setAttribute('viewBox','0 0 '+G.digits*100+' 178');
  $('svg').innerHTML=s;
}
function paint(){
  draw();
  $('nd').value=G.digits;$('ndv').textContent=G.digits;
  $('lp').value=G.ledsPerSeg;$('lpv').textContent=G.ledsPerSeg;
  let s='',d;
  for(d=0;d<G.digits;d++)s+='<button data-d='+d+' aria-pressed='+!!(G.dpMask&(1<<d))+'>'+d+'</button>';
  $('dp').innerHTML=s;
  $('bid').setAttribute('aria-pressed',mode==1);
  $('blr').setAttribute('aria-pressed',mode==2);
  $('nav').style.display=mode==2?'flex':'none';
  $('hint').innerHTML=mode==2?
    '<b>Group '+step+' of '+G.digits*8+'</b> is lit (LED '+step*G.ledsPerSeg+
    '+). Click the segment that lit up, or Skip if nothing did. Clock is paused.':
   mode==1?'Click any segment and it stays lit on the display. Clock is paused.':
   'Identify tests the map you have. Learn wiring builds it from nothing.';
}
function get(u,ok){
  if(busy)return;busy=1;
  fetch(u).then(r=>r.json()).then(j=>{busy=0;ok(j)})
   .catch(()=>{busy=0;say('Clock did not answer. Try that again.')});
}
const flat=()=>{let a=[],d,i,t=tab();
  for(d=0;d<G.digits;d++)for(i=0;i<8;i++)a.push(t[d]&&t[d][i]!=null?t[d][i]:AB);
  return a.join(',')};
const push=()=>get('/setgeometry?digits='+G.digits+'&ledsPerSeg='+G.ledsPerSeg+'&dpMask='+G.dpMask+'&segBase='+flat(),
  j=>{say(j.ok?'Saved.':'Rejected: '+(j.err||'unknown'));if(!j.ok)load()});
const load=()=>get('/geometry',j=>{G=j;G.segBase=G.segBase||[];say('Loaded.');paint()});
const blank=()=>{let a=[],d;for(d=0;d<G.digits;d++)a.push([AB,AB,AB,AB,AB,AB,AB,AB]);return a};
$('nd').oninput=e=>{G.digits=+e.target.value;
  while(G.segBase.length<G.digits)G.segBase.push([AB,AB,AB,AB,AB,AB,AB,AB]);
  paint();push()};
$('lp').oninput=e=>{G.ledsPerSeg=+e.target.value;paint();push()};
$('dp').onclick=e=>{if(e.target.dataset.d===undefined)return;
  G.dpMask^=1<<+e.target.dataset.d;paint();push()};
$('svg').onclick=e=>{
  const id=e.target.id;if(!id||id[0]!='p')return;
  const p=id.substr(1).split('_'),d=+p[0],s=+p[1];
  if(mode==1){
    [].forEach.call(document.querySelectorAll('#svg .hot'),x=>x.classList.remove('hot'));
    e.target.classList.add('hot');
    lastD=d;lastS=s;
    get('/identify?d='+d+'&s='+s,()=>say('Digit '+d+', segment '+NM[s]+' -- still lit.'));
    return}
  if(mode!=2)return;
  tbl[d][s]=step*G.ledsPerSeg;step++;advance();
};
function advance(){
  if(step>=G.digits*8){mode=0;G.segBase=tbl;tbl=null;fetch('/probe?i=-1').catch(()=>{});paint();push();return}
  paint();
  get('/probe?i='+step,j=>{if(j&&j.ok===false)say('Probe refused: '+(j.err||'?'))});
}
$('bid').onclick=()=>{mode=mode==1?0:1;fetch('/probe?i=-1').catch(()=>{});paint()};
$('blr').onclick=()=>{mode=2;step=0;tbl=blank();advance()};
$('bstop').onclick=()=>{mode=0;tbl=null;fetch('/probe?i=-1').catch(()=>{});load()};
$('bskip').onclick=()=>{step++;advance()};
$('bback').onclick=()=>{if(step<1)return;step--;
  let d,i;for(d=0;d<G.digits;d++)for(i=0;i<8;i++)if(tbl[d][i]==step*G.ledsPerSeg)tbl[d][i]=AB;
  advance()};
$('brst').onclick=()=>{let n=0,d,i,a=blank();
  for(d=0;d<G.digits;d++){for(i=0;i<7;i++)a[d][i]=n++*G.ledsPerSeg;
    if(G.dpMask&(1<<d))a[d][7]=n++*G.ledsPerSeg}
  G.segBase=a;mode=0;tbl=null;paint();push()};
setInterval(()=>{if(!mode)return;
  if(mode==2)fetch('/probe?i='+step).catch(()=>{});
  else fetch('/identify?d='+lastD+'&s='+lastS).catch(()=>{});},120000);
addEventListener('pagehide',()=>{if(mode)fetch('/probe?i=-1',{keepalive:true}).catch(()=>{})});
load();
</script>
)HTML";
