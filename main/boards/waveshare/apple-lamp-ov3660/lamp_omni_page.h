#pragma once
inline constexpr const char* kLampOmniPage = R"omnipage(
<details open style="grid-column:1/-1"><summary>Omni Realtime · 实验版</summary>
<p>ESP 直接连接百炼，不经过电脑。先用“开始录音 → 结束并回复”；VAD 自动判断说完，取消会断开本次会话。</p>
<p id="omni-capability">正在读取本固件能力。</p>
<p class="notice">开发版将凭证保存到设备 NVS，重启后恢复；点击清除 Key 可删除。配置页使用局域网 HTTP，请在可信网络使用专用测试 Key。网页状态和日志不回显 Key。</p>
<label>业务空间 Workspace ID <input id="omni-workspace" maxlength="128" autocomplete="off"></label>
<label>地域 <select id="omni-region"><option value="cn-beijing">北京</option><option value="ap-southeast-1">新加坡</option></select></label>
<label>百炼 API Key <input id="omni-key" type="password" maxlength="512" autocomplete="new-password"></label>
<div class="buttons"><button data-omni="configure">保存设备配置</button><button data-omni="start">开始录音</button><button data-omni="finish">结束并回复</button><button data-omni="vad">开始 VAD 对话</button><button data-omni="cancel">取消本轮</button><button data-omni="close">断开会话</button><button data-omni="clear">清除 Key</button></div>
<pre id="omni-status">尚未配置</pre><details><summary>Runtime 状态（新固件）</summary><pre id="runtime-status">尚未读取</pre></details></details>
<script>
window.addEventListener('DOMContentLoaded',()=>{
 const out=document.getElementById('omni-status');
 async function request(body){const r=await fetch('/api/lamp/omni',{method:body?'POST':'GET',headers:{'X-Lamp-Token':token,'Content-Type':'application/json'},body:body?JSON.stringify(body):undefined}); const j=await r.json(); out.textContent=JSON.stringify(j,null,2); document.getElementById('omni-capability').textContent=j.cloud_motion_enabled?'本固件已接入语义动作；上电不使能，先明确上力并退出网页手动控制。图片、陪伴与语音动作同步尚未开放。':'本固件为 PoC：只支持状态与结束对话，不支持云端运动。'; if(j.cloud_motion_enabled){try{const r=await fetch('/api/lamp/runtime',{headers:{'X-Lamp-Token':token}});document.getElementById('runtime-status').textContent=JSON.stringify(await r.json(),null,2)}catch(e){document.getElementById('runtime-status').textContent=e.message}} if(j.ok===false)throw new Error(j.error?.message||'请求失败');return j;}
 document.querySelectorAll('[data-omni]').forEach(b=>b.addEventListener('click',async()=>{b.disabled=true;try{let body={op:b.dataset.omni};if(body.op==='configure'){body.workspace=document.getElementById('omni-workspace').value.trim();body.region=document.getElementById('omni-region').value;body.key=document.getElementById('omni-key').value.trim();}await request(body);}catch(e){out.textContent=e.message;}finally{document.getElementById('omni-key').value='';b.disabled=false;}}));
 request().catch(e=>out.textContent=e.message);setInterval(()=>request().catch(()=>{}),2500);
});
</script>
)omnipage";
