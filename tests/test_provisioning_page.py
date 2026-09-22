"""Exercise the actual embedded page against success, failure and timeout replies."""
import ast
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


class ProvisioningPageTests(unittest.TestCase):
    def test_buttons_recover_and_async_results_are_consumed(self):
        node=shutil.which('node')
        if not node:self.skipTest('Node is needed for embedded JavaScript tests')
        root=Path(__file__).resolve().parents[1]
        source=(root/'main/provisioning.c').read_text(encoding='utf-8')
        block=source.split('static const char INDEX_PAGE[] =',1)[1].split('static esp_err_t http_get_index',1)[0]
        html=''.join(ast.literal_eval(t) for t in re.findall(r'"(?:\\.|[^"\\])*"',block))
        script=html.split('<script>',1)[1].split('</script>',1)[0]
        harness=r'''
const vm=require('node:vm'), assert=require('node:assert/strict');
const source=JSON.parse(process.argv[2]);
async function test(mode){
 const elements={};for(const id of ['status','scan','ssid','pass','nameplate','volume','volumeLabel'])elements[id]={disabled:false,value:'',textContent:'',parentNode:{insertBefore(){}}};
 const context={URLSearchParams,AbortController,Date,Promise,console,
 document:{getElementById:id=>elements[id],createElement:()=>({appendChild(){},remove(){}})},
 setTimeout:(fn,ms)=>{if(ms<5000 || mode==='timeout')queueMicrotask(fn);return 1;},clearTimeout(){},
 fetch:async(path,options)=>{
   if(mode==='timeout')return await new Promise((resolve,reject)=>options.signal.addEventListener('abort',()=>reject(Object.assign(new Error(),{name:'AbortError'}))));
   let data={nameplate:'SMN-AAAA-BBBB',volume:65,save_state:'error',message:'连接失败'};
   if(path==='/scan/start')data={ok:true};
   if(path==='/scan')data=mode==='scan-error'?{state:'error',items:[]}:{state:'done',items:[{ssid:'test "network"',rssi:-40}]};
   if(path==='/save')data=mode==='save-error'?{ok:false,message:'格式错误'}:{ok:true,pending:true};
   return {ok:true,json:async()=>data};
 }};
 vm.createContext(context);vm.runInContext(source,context);await new Promise(resolve=>setImmediate(resolve));
 await context.doScan();assert.equal(elements.scan.disabled,false);
 if(mode==='ok')assert.match(elements.status.textContent,/扫描完成/);
 if(mode==='timeout')assert.match(elements.status.textContent,/超时/);
 if(mode==='scan-error')assert.match(elements.status.textContent,/扫描失败/);
 const button={disabled:false};await context.doSave(button);assert.equal(button.disabled,false);
 if(mode==='save-error')assert.match(elements.status.textContent,/格式错误/);
 if(mode==='ok')assert.match(elements.status.textContent,/连接失败/);
}
(async()=>{for(const mode of ['ok','scan-error','save-error','timeout'])await test(mode);console.log('Provisioning page: PASS');})().catch(e=>{console.error(e);process.exitCode=1;});
'''
        import json
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'test.cjs';path.write_text(harness,encoding='utf-8')
            subprocess.run([node,str(path),json.dumps(script)],check=True,timeout=10)


if __name__=='__main__':unittest.main()
