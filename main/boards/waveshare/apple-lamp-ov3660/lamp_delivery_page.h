#pragma once
inline constexpr const char* kLampDeliveryMarkup=R"delivery(
<div style="grid-column:1/-1">
<section><h2>小智交付版 · 增量示教与跟书</h2><p id="d-message" style="white-space:pre-wrap">等待状态…</p><small>基于小智0.1；没有 Edge Runtime。上电不自动上力、不自动跟书。先校准方向与偏置。</small></section>
<details open><summary>卸力增量示教</summary>
<p class="notice">扶住灯臂后开始；等待五轴卸力确认，再手掰、停稳、记录。关键帧草稿仅在本次开机内；保存动作才写Flash。第1帧是基准，不作为播放时的绝对目标。</p>
<div class="buttons"><button id="d-teach">开始示教并卸力</button><button id="d-capture">记录当前位置</button><button id="d-end">结束示教</button><label>新段时长 s <input id="d-time" type="number" min="0.2" max="10" step="0.1" value="1"></label></div>
<div id="d-frames"></div><button id="d-times">应用各段时长</button>
<div class="row" style="flex-wrap:wrap;margin-top:16px"><label>动作ID（英文/数字/短横线）<input id="d-id" maxlength="40" value="my-nod" style="width:210px"></label><label>名称<input id="d-name" maxlength="32" value="轻轻点头" style="width:180px"></label></div>
<label>动作描述（供模型选择）<textarea id="d-description" maxlength="256" style="width:100%;min-height:60px">轻轻点头，表示同意或回应。</textarea></label>
<p>参与关节（未选关节的增量为0）：</p><div id="d-joints" class="buttons"></div>
<label>默认结束方式 <select id="d-default-end"><option value="return_to_reference">回本次起点</option><option value="hold_at_end">保持最后姿态</option></select></label>
<button id="d-save" class="primary">保存增量动作到Flash</button><p id="d-teaching-status"></p>
</details>
<details open><summary>Flash动作库与编排</summary>
<p id="d-store"></p><button id="d-format" class="danger">首次初始化／清空动作分区</button><small>仅擦用户动作分区，旧动作会丢失；挂载失败不会自动格式化。先进入手动并卸力。</small>
<div class="buttons"><select id="d-library"></select><button id="d-play">播放选中动作</button><button id="d-export">导出／编辑选中动作</button><button id="d-delete" class="danger">删除选中动作</button></div>
<label>本次播放结束 <select id="d-play-end"><option value="return_to_reference">回本次起点</option><option value="hold_at_end">保持最后姿态</option></select></label>
<p>试播前结束示教，在原面板明确使能。播放不回录制首帧；超限会拒绝，不自动缩放。</p>
<label>编排动作ID，逗号分隔（最多16项／60秒）<input id="d-group" style="width:100%" placeholder="my-nod,my-tilt"></label><button id="d-play-group">播放动作组</button>
<label>动作JSON（可编辑描述、增量与时长；不得擅自更改binding）<textarea id="d-json" style="width:100%;min-height:130px"></textarea></label>
<div class="buttons"><button id="d-download">下载编辑框JSON</button><input type="file" id="d-file" accept=".json,application/json"><button id="d-import">校验并导入／更新</button></div>
</details>
<details open><summary>低频跟书 · 先观察再运动</summary>
<p>先唤醒小智，让服务下发视觉地址。先点“只观察一次”检查图片与定位；书放在可见区域，首次须核对图像左右/上下对应的URDF调整方向。姿态可从原面板调好。不大范围搜索，只调Yaw与灯头Pitch。</p>
<div class="buttons"><button id="d-observe">只观察一次（不运动）</button><button id="d-enable-track">切换自主控制并保持当前位置使能</button><button id="d-start-track">开始跟书</button><button id="d-stop-track" class="danger">停止跟书（保留力矩）</button></div>
<div id="d-tuning" class="buttons"></div><button id="d-configure">确认图像方向并保存跟书参数</button>
<p id="d-tracking-status"></p><img id="d-image" alt="最近一次实际上传图片" style="max-width:100%;width:320px;display:none"><pre id="d-observation"></pre>
<p class="muted">间隔是上一轮结束后的等待，不含模型与运动耗时。连续3次请求／解析失败暂停；用户动作、卸力和手动接管会停止跟书，需明确重启。停止会立即使旧结果失效，HTTP请求可稍后结束。</p>
</details>
<details><summary>交付诊断</summary><pre id="d-raw"></pre></details>
</div>
)delivery";
inline constexpr const char* kLampDeliveryScript=R"delivery(
<script>
let ds=null,dFramesKey='',dCatalogKey='',dPreviewId=0,dImageUrl='',dRefreshing=false,dTuningLoaded=false;
const dFields=[['interval_sec','间隔s',3,30,1],['gain_deg','比例增益°',1,20,1],['deadzone','画面死区',.02,.25,.01],['max_step_deg','单步上限°',.2,3,.1],['duration_sec','运动时长s',.5,5,.1],['confidence','置信门槛',.5,1,.05]];
for(let i=0;i<5;i++){const label=document.createElement('label'),input=document.createElement('input');input.type='checkbox';input.id='d-joint-'+i;input.checked=i===3;label.append(input,document.createTextNode(names[i]));$('d-joints').append(label)}
for(const [key,label,min,max,step] of dFields){const l=document.createElement('label'),input=document.createElement('input');input.type='number';input.id='d-opt-'+key;Object.assign(input,{min,max,step});l.append(document.createTextNode(label+' '),input);$('d-tuning').append(l)}
for(const [key,label] of [['yaw_sign','书在画面右侧，Yaw调整符号'],['pitch_sign','书在画面下侧，灯头Pitch调整符号']]){const l=document.createElement('label'),s=document.createElement('select');s.id='d-opt-'+key;for(const [v,t] of [[0,'未确认'],[1,'+1'],[-1,'−1']]){const o=document.createElement('option');o.value=v;o.textContent=t;s.append(o)}l.append(document.createTextNode(label+' '),s);$('d-tuning').append(l)}
function dTell(t){$('d-message').textContent=t}
async function dRun(fn){try{await fn();await dRefresh()}catch(e){dTell(String(e));log('delivery_error',String(e))}}
async function dCommand(operation,args={}){
 const request_id='delivery-'+Date.now().toString(36)+'-'+(++requestCounter);
 log('delivery_request',{request_id,operation,args});let receipt=await api('delivery',{operation,request_id,args});
 if(receipt.job_id){dTell('已受理：'+receipt.job_id+'；等待本地处理…');let finished=false;
  for(let i=0;i<150;i++){await new Promise(r=>setTimeout(r,800));const job=await api('delivery/jobs/'+receipt.job_id);if(['completed','failed'].includes(job.status)){receipt=job.result;finished=true;break}}
  if(!finished)throw Error('处理等待超时，请查看日志与原job，不要重复提交');
 }
 if(receipt.ok===false)throw Error(receipt.error?.message||'处理失败');
 if(receipt.result?.task_id){const task_id=receipt.result.task_id;dTell('处理完成，机械任务仍在执行：'+task_id);
  let finished=false;for(let i=0;i<150;i++){await new Promise(r=>setTimeout(r,700));const task=await api('tasks/'+task_id);if(['completed','failed','cancelled','limited'].includes(task.result.status)){log('delivery_task',task.result);finished=true;if(task.result.status!=='completed')throw Error(task.result.reason);break}}
  if(!finished)throw Error('机械任务等待超时，不要重复播放；检查真实反馈');
 }
 log('delivery_result',receipt);dTell('完成：'+operation);return receipt;
}
async function dRefresh(){if(dRefreshing)return;dRefreshing=true;try{
 ds=await api('delivery');$('d-teaching-status').textContent='示教状态：'+ds.teaching_status+'；'+(ds.frames?.length||0)+'帧';
 $('d-store').textContent=(ds.storage_ready?'动作分区已挂载':'动作分区不可用')+' · '+ds.storage_used+'/'+ds.storage_total+' bytes · '+(ds.storage_error||'');
 const fk=JSON.stringify(ds.frames||[]);if(fk!==dFramesKey){dFramesKey=fk;$('d-frames').replaceChildren();(ds.frames||[]).forEach((f,i)=>{
  const row=document.createElement('div');row.className='cal-row';const txt=document.createElement('p');txt.textContent=(i===0?'基准帧':'关键帧'+(i+1))+' · '+Object.entries(f.pose_rad).map(([k,v])=>k+':'+(v*D).toFixed(1)+'°').join(' / ');
  const input=document.createElement('input');input.type='number';input.min=.2;input.max=10;input.step=.1;input.value=f.duration_sec;input.dataset.dFrame=i;input.disabled=i===0;
  const del=document.createElement('button');del.textContent='删除';del.onclick=()=>dRun(()=>dCommand('remove_frame',{index:i}));row.append(txt,document.createTextNode('从上一帧运动时长s：'),input,del);$('d-frames').append(row)
 })}
 const ck=JSON.stringify(ds.actions||[]);if(ck!==dCatalogKey){dCatalogKey=ck;const old=$('d-library').value;$('d-library').replaceChildren();for(const a of ds.actions||[]){const o=document.createElement('option');o.value=a.id;o.textContent=a.id+' · '+(a.name||'损坏文件')+(a.valid&&a.compatible?'':' [不可播放]');$('d-library').append(o)}if([...$('d-library').options].some(o=>o.value===old))$('d-library').value=old}
 if(!dTuningLoaded&&ds.tracking_options){for(const [k,v]of Object.entries(ds.tracking_options)){const el=$('d-opt-'+k);if(el)el.value=v}dTuningLoaded=true}
 $('d-tracking-status').textContent=(ds.tracking_enabled?'跟书开启':'跟书关闭')+' · '+ds.tracking_status+' · frame '+ds.frame_id+' · 观察年龄 '+ds.observation_age_ms+'ms · '+(ds.error||'');
 $('d-observation').textContent=JSON.stringify({observation:ds.observation,capture_pose_rad:ds.capture_pose_rad,raw:ds.vision_response_preview},null,2);$('d-raw').textContent=JSON.stringify(ds,null,2);
 $('d-capture').disabled=ds.teaching_status!=='recording';$('d-save').disabled=!ds.storage_ready||ds.teaching_status!=='recording'||(ds.frames?.length||0)<2;$('d-start-track').disabled=!ds.tracking_configured||ds.tracking_enabled;
 if(ds.frame_id&&ds.frame_id!==dPreviewId&&ds.tracking_status!=='observing'){
  const r=await fetch('/api/lamp/delivery/jpeg',{headers:{'X-Lamp-Token':token}});if(r.headers.get('Content-Type')?.startsWith('image/jpeg')){const url=URL.createObjectURL(await r.blob());if(dImageUrl)URL.revokeObjectURL(dImageUrl);dImageUrl=url;$('d-image').src=url;$('d-image').style.display='block';dPreviewId=ds.frame_id}
 }
}catch(e){$('d-raw').textContent=String(e)}finally{dRefreshing=false}}
$('d-teach').onclick=()=>dRun(async()=>{if(confirm('扶住灯臂。将停止当前运动并卸力，清空本次草稿，开始示教？'))await dCommand('teach_start',{discard_previous:true})});
$('d-capture').onclick=()=>dRun(()=>dCommand('capture',{duration_sec:+$('d-time').value}));
$('d-end').onclick=()=>dRun(()=>dCommand('teach_end'));
$('d-times').onclick=()=>dRun(()=>dCommand('set_times',{duration_sec:[...document.querySelectorAll('[data-d-frame]')].map(e=>+e.value)}));
$('d-save').onclick=()=>dRun(()=>dCommand('save',{id:$('d-id').value,name:$('d-name').value,description:$('d-description').value,joints:[0,1,2,3,4].map(i=>$('d-joint-'+i).checked),end_mode:$('d-default-end').value}));
$('d-format').onclick=()=>dRun(async()=>{if(confirm('仅清空用户动作分区。已有用户动作将丢失；确认已导出备份，且设备处于手动卸力状态？'))await dCommand('format',{confirm_erase_actions:true})});
$('d-play').onclick=()=>dRun(()=>dCommand('play',{id:$('d-library').value,end_mode:$('d-play-end').value}));
$('d-play-group').onclick=()=>dRun(()=>dCommand('play_group',{ids:$('d-group').value.split(',').map(s=>s.trim()).filter(Boolean),end_mode:$('d-play-end').value}));
$('d-export').onclick=()=>dRun(async()=>{const r=await dCommand('export',{id:$('d-library').value});$('d-json').value=JSON.stringify(r.action,null,2)});
$('d-delete').onclick=()=>dRun(async()=>{if(confirm('删除动作 '+$('d-library').value+'？'))await dCommand('delete',{id:$('d-library').value})});
$('d-import').onclick=()=>dRun(()=>dCommand('import',{action:JSON.parse($('d-json').value)}));
$('d-file').onchange=()=>dRun(async()=>{const f=$('d-file').files[0];if(!f)return;if(f.size>12288)throw Error('文件超过12KiB');$('d-json').value=await f.text()});
$('d-download').onclick=()=>dRun(async()=>{const data=JSON.parse($('d-json').value),a=document.createElement('a');const url=URL.createObjectURL(new Blob([JSON.stringify(data,null,2)],{type:'application/json'}));a.href=url;a.download='lamp-action-'+(data.id||'export')+'.json';a.click();setTimeout(()=>URL.revokeObjectURL(url),1000)});
$('d-observe').onclick=()=>dRun(()=>dCommand('observe'));
$('d-enable-track').onclick=()=>dRun(async()=>{if(confirm('结束手动控制，按实际位置上力，供跟书使用。已检查安装映射和限位？'))await dCommand('enable_tracking_control')});
$('d-start-track').onclick=()=>dRun(()=>dCommand('track_start'));
$('d-stop-track').onclick=()=>dRun(()=>dCommand('track_stop'));
$('d-configure').onclick=()=>dRun(async()=>{if(confirm('已用小幅测试核对图像坐标与实际转动方向？')){const args={};for(const [k]of dFields)args[k]=+$('d-opt-'+k).value;for(const k of ['yaw_sign','pitch_sign'])args[k]=+$('d-opt-'+k).value;await dCommand('configure_tracking',args)}});
(async()=>{while(true){await dRefresh();await new Promise(r=>setTimeout(r,1200))}})();
</script>
)delivery";
