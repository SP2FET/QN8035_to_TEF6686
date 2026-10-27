// Execute the page's actual functions with controlled DOM, network and timer fixtures.
const fs=require('fs'),path=require('path'),assert=require('assert');
const html=fs.readFileSync(path.resolve(__dirname,'../../components/web_ui/www/index.html'),'utf8');
const ids=[...html.matchAll(/\bid="([^"]+)"/g)].map(m=>m[1]);
assert.equal(new Set(ids).size,ids.length,'duplicate element IDs');
new Function(html.match(/<script>([\s\S]*?)<\/script>/)[1]);
let queue=[],partial='',logDropped=0,paused=true;
const MAXLINES=2000,ANSI=/\x1b\[[0-9;]*m/g;
function sched(){} function count(){}
const addText=eval('('+html.match(/function addText\(t\)\{[\s\S]*?\n\}/)[0]+')');
addText('line\n'.repeat(10000));
assert.equal(queue.length,MAXLINES);assert.equal(logDropped,8000);
addText('x'.repeat(20000));assert(partial.length<=4096);

let tun={params:[{k:'gain',v:10,sv:0}]},serverLive=0,persisted=0;
const elements={tsave:{},qsave:{}};
function $(id){return elements[id]}
function toast(m){throw new Error(m)}
async function cmd(p,v){
  if(p==='tuning'){await new Promise(r=>setTimeout(r,5));serverLive=v.v}
  else if(p==='tuning/save')persisted=serverLive;
  return {ok:true};
}
async function loadTun(){tun.params[0].sv=persisted}
const helpers=html.slice(html.indexOf('let tuningWork='),html.indexOf('const dec=p=>'));
const save=html.match(/\$\('tsave'\)\.onclick=\$\('qsave'\)\.onclick=(async\(\)=>\{[\s\S]*?\n\});/)[1];
// A single eval retains the helpers' lexical state for the handler and timer.
eval(helpers+`
(async()=>{
  const handler=(${save});
  deferTuning(tun.params[0],()=>queueTuning(()=>cmd('tuning',{v:10})),250);
  await handler();
  assert.equal(persisted,10);assert.equal(tun.params[0].sv,10);
  assert.equal(elements.tsave.disabled,false);
  console.log('PASS debounce flushed and applied before Save; actual saved snapshot displayed');
})().catch(e=>{console.error(e);process.exitCode=1});
`);
