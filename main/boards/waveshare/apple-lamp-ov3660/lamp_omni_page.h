#pragma once
inline constexpr const char* kLampOmniPage = R"omnipage(
<details open style="grid-column:1/-1"><summary>Omni Realtime · 实验版</summary>
<p>ESP 直接连接百炼，不经过电脑。第一轮先用“开始录音 → 结束并回复”；VAD 模式自动判断说完。取消会断开本次会话。云端仅能查询台灯状态，暂不能运动。</p>
<p class="notice">凭证仅在本次开机的 RAM 中保存。配置页使用局域网 HTTP，请只在可信网络使用专用测试 Key，不要在共享网络填写生产凭证。刷新不清除；重启清除。</p>
<label>业务空间 Workspace ID <input id="omni-workspace" maxlength="128" autocomplete="off"></label>
<label>地域 <select id="omni-region"><option value="cn-beijing">北京</option><option value="ap-southeast-1">新加坡</option></select></label>
<label>百炼 API Key <input id="omni-key" type="password" maxlength="512" autocomplete="new-password"></label>
<div class="buttons"><button data-omni="configure">保存本次开机配置</button><button data-omni="start">开始录音</button><button data-omni="finish">结束并回复</button><button data-omni="vad">开始 VAD 对话</button><button data-omni="cancel">取消本轮</button><button data-omni="close">断开会话</button><button data-omni="clear">清除 Key</button></div>
<pre id="omni-status">尚未配置</pre></details>
<script>
window.addEventListener('DOMContentLoaded',()=>{
 const out=document.getElementById('omni-status');
 async function request(body){const r=await fetch('/api/lamp/omni',{method:body?'POST':'GET',headers:{'X-Lamp-Token':token,'Content-Type':'application/json'},body:body?JSON.stringify(body):undefined}); const j=await r.json(); out.textContent=JSON.stringify(j,null,2); if(j.ok===false)throw new Error(j.error?.message||'请求失败');return j;}
 document.querySelectorAll('[data-omni]').forEach(b=>b.addEventListener('click',async()=>{b.disabled=true;try{let body={op:b.dataset.omni};if(body.op==='configure'){body.workspace=document.getElementById('omni-workspace').value.trim();body.region=document.getElementById('omni-region').value;body.key=document.getElementById('omni-key').value.trim();}await request(body);}catch(e){out.textContent=e.message;}finally{document.getElementById('omni-key').value='';b.disabled=false;}}));
 request().catch(e=>out.textContent=e.message);setInterval(()=>request().catch(()=>{}),2500);
});
</script>
)omnipage";
