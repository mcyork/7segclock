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
//
// 1.2.0: requests are queued, never dropped (a dropped final save lost a whole
// wizard run); a save is judged by the reply's ok field, so "Saved." means saved;
// turning a decimal point off clears its table cell; changing LEDs-per-segment
// rescales the learned table instead of sending one that overlaps itself; the
// shape controls lock while Learn is running.
#pragma once
#include <Arduino.h>

const char GEOMETRY_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang=en>
<meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>mini7seg geometry</title>
<style>
:root{--bg:#0d0f13;--fg:#e8eaed;--mut:#8b929c;--line:#252a32;--acc:#00a0ff;--bad:#ff6b6b}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,-apple-system,sans-serif}
.w{max-width:26rem;margin:0 auto;padding:1.5rem 1.1rem 3rem}
h1{font-size:1.1rem;margin:0 0 .2rem}
.sub{color:var(--mut);font-size:.82rem;margin:0 0 1.6rem;min-height:1.2em}
.sub.bad{color:var(--bad)}
fieldset{border:0;padding:0;margin:0 0 1.5rem}
fieldset[disabled]{opacity:.45}
legend{font-size:.72rem;text-transform:uppercase;letter-spacing:.09em;color:var(--mut);padding:0 0 .5rem}
.modes{display:grid;grid-template-columns:repeat(2,1fr);gap:.5rem}
.modes button{padding:.7rem .5rem;border:1px solid var(--line);background:#151920;color:var(--fg);
 border-radius:.5rem;font:inherit;font-size:.9rem;cursor:pointer;transition:.12s}
.modes button[aria-pressed=true]{background:var(--acc);border-color:var(--acc);color:#04121d;font-weight:600}
.row{display:flex;align-items:center;gap:.8rem;padding:.55rem 0}
.row label{flex:1;font-size:.92rem}
.row output{color:var(--mut);font-variant-numeric:tabular-nums;font-size:.85rem;min-width:2.6rem;text-align:right}
input[type=range]{flex:2;accent-color:var(--acc)}
input[type=number]{flex:2;background:#151920;color:var(--fg);border:1px solid var(--line);border-radius:.4rem;padding:.45rem;font:inherit}
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

<fieldset id=shape><legend>Shape</legend>
<div class=row><label for=nd>Digits</label><input type=range id=nd min=1 max=8><output id=ndv></output></div>
<div class=row><label for=lp>LEDs per segment</label><input type=range id=lp min=1 max=8><output id=lpv></output></div>
<div class=row><label for=sl>Total LEDs on strip</label><input type=number id=sl min=0 max=512><output id=slv></output></div>
<div class=row><label id=dpl>Decimal points</label><div class=seg id=dp aria-labelledby=dpl></div></div>
</fieldset>

<fieldset><legend>Map</legend>
<svg id=svg role=img aria-label="the display, one polygon per segment"></svg>
<p class=note id=hint style="border:0;padding:.7rem 0 0;margin:0"></p>
<div class=modes id=act style="margin-top:.6rem">
<button id=bid>Identify</button><button id=blr>Learn wiring</button></div>
<div class=seg id=nav style="margin-top:.5rem;display:none">
<button id=bback>Back</button><button id=bskip>Skip</button>
<button id=bdone>Finish</button><button id=bstop>Cancel</button></div>
</fieldset>

<fieldset id=rst><legend>Reset</legend>
<div class=seg><button id=brst>Standard wiring order</button><button id=bdef>Factory shape</button></div>
<p class="note" style="border:0;padding:.5rem 0 0;margin:0"><b>Standard wiring order</b> keeps your digit count, LEDs per segment and decimal
points and assigns LEDs in the order A,B,C,D,E,F,G,DP per digit. <b>Factory shape</b> goes all the way back to the four-digit,
one-LED-per-segment panel this firmware ships for.</p>
</fieldset>

<p class=note id=note>Learn mode lights one LED group at a time and asks which segment it was, so you never have to know your own wiring. The picture fills in as you go.</p>
<p class=note style="border:0"><a href="/" style="color:var(--acc)">&larr; back to settings</a></p>
</div>
<script>
const $=i=>document.getElementById(i);
const AB=65535;   // absent segment; must match SEGMENT_ABSENT in settings.h
const NM=['A top','B top right','C bottom right','D bottom','E bottom left','F top left','G middle','DP'];
const H={method:'POST',headers:{'X-7seg':'1'}};
let G={digits:4,ledsPerSeg:1,dpMask:0,segBase:[]},mode=0,step=0,tbl=null,lastD=-1,lastS=-1;
const Hx=(x,y,w)=>[[x,y+7],[x+7,y],[x+w-7,y],[x+w,y+7],[x+w-7,y+14],[x+7,y+14]].join(' ');
const V=(x,y,h)=>[[x+7,y],[x+14,y+7],[x+14,y+h-7],[x+7,y+h],[x,y+h-7],[x,y+7]].join(' ');
const P=[Hx(6,6,72),V(64,22,49),V(64,89,49),Hx(6,140,72),V(6,89,49),V(6,22,49),Hx(6,73,72)];
const say=(t,bad)=>{const s=$('stat');s.textContent=t;s.className='sub'+(bad?' bad':'')};
const tab=()=>mode==2?tbl:G.segBase;
const has=(d,s)=>{const t=tab()[d];return t&&t[s]!=AB?'on':''};
// Finished means every segment has an LED, not every LED has been offered.
const need=()=>{let n=G.digits*7,d;for(d=0;d<G.digits;d++)if(G.dpMask&(1<<d))n++;return n};
const got=()=>{let n=0,d,i,t=tab();for(d=0;d<G.digits;d++)for(i=0;i<8;i++)
  if(t[d]&&t[d][i]!=null&&t[d][i]!=AB)n++;return n};
const groups=()=>Math.max(1,Math.floor((G.ledCount||G.digits*8*G.ledsPerSeg)/G.ledsPerSeg));
function draw(){
  let s='',d,i;
  for(d=0;d<G.digits;d++){
    s+='<g transform="translate('+d*100+',0)">';
    for(i=0;i<7;i++)s+='<polygon class="'+has(d,i)+'" id="p'+d+'_'+i+'" points="'+P[i]+'"/>';
    if(G.dpMask&(1<<d))s+='<circle class="'+has(d,7)+'" id="p'+d+'_7" cx=90 cy=147 r="7"/>';
    s+='<text x=40 y=172 text-anchor=middle>'+(d+1)+'</text></g>';
  }
  $('svg').setAttribute('viewBox','0 0 '+G.digits*100+' 178');
  $('svg').innerHTML=s;
}
function paint(){
  draw();
  $('nd').value=G.digits;$('ndv').textContent=G.digits;
  $('lp').value=G.ledsPerSeg;$('lpv').textContent=G.ledsPerSeg;
  $('sl').value=G.stripLen||0;
  $('slv').textContent=G.stripLen?G.ledCount+' used':'auto ('+(G.ledCount||0)+')';
  let s='',d;
  for(d=0;d<G.digits;d++)s+='<button data-d='+d+' aria-pressed='+!!(G.dpMask&(1<<d))+'>'+(d+1)+'</button>';
  $('dp').innerHTML=s;
  $('bid').setAttribute('aria-pressed',mode==1);
  $('blr').setAttribute('aria-pressed',mode==2);
  $('nav').style.display=mode==2?'flex':'none';
  // Changing the shape mid-wizard would leave the table short and cancel the lit
  // group; Identify mid-wizard would drop the table without a save.
  $('shape').disabled=mode==2;$('rst').disabled=mode!=0;$('bid').disabled=mode==2;
  $('hint').innerHTML=mode==2?
    '<b>Group '+(step+1)+' of '+groups()+'</b> is lit (LED '+step*G.ledsPerSeg+
    (G.ledsPerSeg>1?'-'+(step*G.ledsPerSeg+G.ledsPerSeg-1):'')+'). Click the segment that lit up, or Skip if nothing did. Clock is paused.'+
    '<br>Mapped <b>'+got()+' of '+need()+'</b> segments.':
   mode==1?'Click any segment and it stays lit on the display. Clock is paused.':
   'Identify tests the map you have. Learn wiring builds it from nothing.';
}
// Serialised, never dropped. The old version silently ignored a request while one
// was in flight, which lost the final save of a finished wizard run.
let Q=Promise.resolve();
function post(u,ok){Q=Q.then(()=>fetch(u,H).then(r=>r.json()).then(ok))
  .catch(()=>say('Clock did not answer. Try that again.',true));return Q}
const flat=()=>{let a=[],d,i,t=tab();
  for(d=0;d<G.digits;d++)for(i=0;i<8;i++)
    a.push(i==7&&!(G.dpMask&(1<<d))?AB:(t[d]&&t[d][i]!=null?t[d][i]:AB));   // a cleared DP sends ABSENT
  return a.join(',')};
// A save is judged by what the firmware said: ok:false is a rejection (reload the
// truth), anything else IS the new geometry.
const push=msg=>post('/setgeometry?digits='+G.digits+'&ledsPerSeg='+G.ledsPerSeg+'&dpMask='+G.dpMask+'&stripLen='+(G.stripLen||0)+'&segBase='+flat(),
  j=>{if(j.ok===false){say('Rejected: '+(j.error||'unknown'),true);load()}else{G=j;G.segBase=G.segBase||[];say(msg||'Saved.');paint()}});
const load=()=>fetch('/geometry').then(r=>r.json()).then(j=>{G=j;G.segBase=G.segBase||[];say('Loaded.');paint()})
  .catch(()=>say('Clock did not answer.',true));
const blank=()=>{let a=[],d;for(d=0;d<G.digits;d++)a.push([AB,AB,AB,AB,AB,AB,AB,AB]);return a};
$('nd').oninput=e=>{$('ndv').textContent=e.target.value};
$('nd').onchange=e=>{G.digits=+e.target.value;
  while(G.segBase.length<G.digits)G.segBase.push([AB,AB,AB,AB,AB,AB,AB,AB]);
  paint();push()};
$('lp').oninput=e=>{$('lpv').textContent=e.target.value};
$('lp').onchange=e=>{const o=G.ledsPerSeg,n=+e.target.value;
  // Rescale the learned order: base indices are group*ledsPerSeg, so keep the group.
  G.segBase=G.segBase.map(r=>r.map(v=>v==AB?AB:Math.round(v/o)*n));G.ledsPerSeg=n;paint();push()};
$('sl').onchange=e=>{G.stripLen=+e.target.value||0;push()};
$('dp').onclick=e=>{if(e.target.dataset.d===undefined)return;const d=+e.target.dataset.d;
  G.dpMask^=1<<d;
  const on=!!(G.dpMask&(1<<d));
  if(!on&&G.segBase[d])G.segBase[d][7]=AB;    // off: forget its LED, or the firmware rejects the table
  paint();
  // On, with no LED known for it yet: the save succeeds but the dot has nothing to light.
  push(on&&(!G.segBase[d]||G.segBase[d][7]==AB)?'Saved. Digit '+(d+1)+' has a decimal point but no LED for it yet - run Learn wiring, or Reset to standard wiring.':undefined)};
$('svg').onclick=e=>{
  const id=e.target.id;if(!id||id[0]!='p')return;
  const p=id.substr(1).split('_'),d=+p[0],s=+p[1];
  if(mode==1){
    [].forEach.call(document.querySelectorAll('#svg .hot'),x=>x.classList.remove('hot'));
    e.target.classList.add('hot');
    lastD=d;lastS=s;
    post('/identify?d='+d+'&s='+s,j=>say(j.ok===false?'Refused: '+(j.error||'?'):'Digit '+(d+1)+', segment '+NM[s]+' -- still lit.',j.ok===false));
    return}
  if(mode!=2)return;
  if(!tbl[d])return;
  tbl[d][s]=step*G.ledsPerSeg;step++;advance();
};
function finish(done){
  const msg=done?'All '+need()+' segments mapped. Clock resumed.':'Stopped with '+got()+' of '+need()+' mapped. Clock resumed.';
  mode=0;G.segBase=tbl;tbl=null;post('/probe?i=-1',()=>{});paint();push(msg);
}
function advance(){
  if(got()>=need()){finish(true);return}          // every segment has an LED
  if(step>=groups()){finish(false);return}        // ran out of strip
  paint();
  post('/probe?i='+step,j=>{if(j&&j.ok===false)say('Probe refused: '+(j.error||'?'),true)});
}
$('bid').onclick=()=>{mode=mode==1?0:1;lastD=-1;lastS=-1;post('/probe?i=-1',()=>{});paint()};
$('blr').onclick=()=>{mode=2;step=0;tbl=blank();advance()};
$('bstop').onclick=()=>{mode=0;tbl=null;post('/probe?i=-1',()=>{});load()};
$('bskip').onclick=()=>{step++;advance()};
$('bdone').onclick=()=>{if(mode==2)finish(got()>=need())};
$('bback').onclick=()=>{if(step<1)return;step--;
  let d,i;for(d=0;d<G.digits;d++)for(i=0;i<8;i++)if(tbl[d][i]==step*G.ledsPerSeg)tbl[d][i]=AB;
  advance()};
$('brst').onclick=()=>{let n=0,d,i,a=blank();
  for(d=0;d<G.digits;d++){for(i=0;i<7;i++)a[d][i]=n++*G.ledsPerSeg;
    if(G.dpMask&(1<<d))a[d][7]=n++*G.ledsPerSeg}
  G.segBase=a;mode=0;tbl=null;paint();push('Standard wiring order applied to this shape.')};
// The firmware's own reset: 4 digits, 1 LED per segment, decimal points on all four, packed.
$('bdef').onclick=()=>{if(!confirm('Back to the factory shape: 4 digits, 1 LED per segment, all decimal points?'))return;
  mode=0;tbl=null;post('/setgeometry?reset=1',j=>{if(j.ok===false){say('Rejected: '+(j.error||'unknown'),true);load()}else{G=j;G.segBase=G.segBase||[];say('Factory shape restored.');paint()}})};
// Keep the latched preview alive while the page is open; only re-send an Identify
// that was actually clicked.
setInterval(()=>{if(!mode)return;
  const u=mode==2?'/probe?i='+step:(lastD>=0?'/identify?d='+lastD+'&s='+lastS:null);
  if(u)post(u,()=>{});},120000);   // through the queue, so it can never overtake a click
addEventListener('pagehide',()=>{if(mode)fetch('/probe?i=-1',Object.assign({keepalive:true},H)).catch(()=>{})});
load();
</script>
</html>
)HTML";
