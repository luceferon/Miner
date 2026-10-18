/*
 * index_html.h
 * Основной веб-интерфейс (дашборд) — STA режим
 *
 * INDEX_HTML — полная страница с:
 *   - заголовком (ESP IP, RSSI, Uptime, счётчик майнеров)
 *   - карточками майнеров (статус, хешрейт, мощность, температуры, вентиляторы, перезагрузка)
 *   - модальным окном логов (ring-buffer, автоскролл, сохранение .log)
 *   - модальным окном настроек (добавление/редактирование/удаление майнеров, сканирование сети)
 *   - блоком OTA-ссылки
 */

#pragma once

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="ru"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Miner Control · ESP32-C6</title>
<style>
*{box-sizing:border-box;margin:0;padding:0;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Consolas,monospace}
body{background:#0a0e1a;color:#e5e9f0;min-height:100vh;padding:20px}
.hdr{max-width:1400px;margin:0 auto 18px;display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:12px}
.hdr h1{font-size:20px;color:#88c0d0;letter-spacing:.5px}
.hdr .meta{display:flex;gap:14px;font-size:13px;color:#81a1c1;flex-wrap:wrap}
.hdr .meta b{color:#a3be8c}
.hdr .nav{display:flex;gap:8px}
.btn{padding:7px 14px;border-radius:6px;font-size:12px;font-weight:600;cursor:pointer;border:none;transition:all .15s;text-transform:uppercase;letter-spacing:.3px}
.btn.ghost{background:transparent;color:#81a1c1;border:1px solid #2a334d}
.btn.ghost:hover{color:#e5e9f0;border-color:#50fa7b}
.btn.ok{background:rgba(80,250,123,.18);color:#50fa7b;border:1px solid rgba(80,250,123,.4)}
.btn.ok:hover{background:rgba(80,250,123,.3)}
.btn.danger{background:rgba(255,106,80,.15);color:#ff6b6b;border:1px solid rgba(255,106,80,.35)}
.btn.danger:hover{background:rgba(255,106,80,.28);transform:translateY(-1px)}
.btn.danger:disabled{opacity:.45;cursor:not-allowed;transform:none}
.btn.primary{background:rgba(136,192,208,.18);color:#88c0d0;border:1px solid rgba(136,192,208,.4)}
.btn.primary:hover{background:rgba(136,192,208,.3)}

.grid{max-width:1400px;margin:0 auto;display:grid;grid-template-columns:repeat(auto-fit,minmax(520px,1fr));gap:16px}
.card{background:#141a2b;border:1px solid #2a334d;border-radius:10px;padding:18px;box-shadow:0 4px 18px rgba(0,0,0,.3)}
.card .title{display:flex;align-items:center;justify-content:space-between;margin-bottom:14px;flex-wrap:wrap;gap:8px}
.card h2{font-size:16px;color:#e5e9f0;font-weight:700}
.card h2 small{font-size:11px;color:#616e88;margin-left:6px;font-weight:400}
.badge{display:inline-flex;align-items:center;gap:6px;padding:3px 10px;border-radius:999px;font-size:11px;font-weight:600}
.badge.ant{background:rgba(139,233,253,.12);color:#8be9fd;border:1px solid rgba(139,233,253,.3)}
.badge.what{background:rgba(180,142,173,.12);color:#b48ead;border:1px solid rgba(180,142,173,.3)}
.status{display:inline-flex;align-items:center;gap:6px;padding:4px 10px;border-radius:999px;font-size:12px;font-weight:600}
.status.online{background:rgba(72,181,128,.15);color:#50fa7b;border:1px solid rgba(80,250,123,.3)}
.status.offline{background:rgba(255,85,85,.12);color:#ff5555;border:1px solid rgba(255,85,85,.3)}
.status.warn{background:rgba(241,250,140,.12);color:#f1fa8c;border:1px solid rgba(241,250,140,.3)}
.dot{width:8px;height:8px;border-radius:50%;background:currentColor;animation:pulse 1.5s infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.35}}
.metrics{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;margin-bottom:12px}
.metric{background:#0e1320;border:1px solid #252d46;border-radius:8px;padding:9px 11px}
.metric .label{font-size:11px;color:#616e88;text-transform:uppercase;letter-spacing:.4px}
.metric .value{font-size:18px;font-weight:700;color:#eceff4;margin-top:2px}
.metric .value.sm{font-size:14px}
.metric.hash .value{color:#8be9fd}
.metric.pwr .value{color:#ffb86c}
.metric.temp .value{color:#ff5555}
.metric.temp.ok .value{color:#50fa7b}
.metric.temp.warn .value{color:#f1fa8c}
.sub{display:grid;grid-template-columns:1fr 1fr;gap:9px;margin-bottom:12px}
.row{background:#0e1320;border:1px solid #252d46;border-radius:8px;padding:7px 11px;font-size:12px;color:#b8c0d0;line-height:1.4}
.row b{color:#e5e9f0;margin-right:6px}
.err{background:rgba(255,85,85,.1);border:1px solid rgba(255,85,85,.3);color:#ff6b6b;padding:9px 11px;border-radius:8px;font-size:12px;margin-bottom:11px;display:none;line-height:1.4}
.err.show{display:block}
.reboot{display:flex;align-items:center;justify-content:space-between;gap:12px;padding-top:11px;border-top:1px dashed #2a334d;flex-wrap:wrap}
.reboot-info{font-size:12px;color:#81a1c1;flex:1;min-width:220px}
.reboot-info.ok{color:#50fa7b}
.reboot-info.fail{color:#ff6b6b}
.fans{display:flex;gap:5px;flex-wrap:wrap;margin-top:3px}
.fan{font-size:10px;background:#1b2238;border:1px solid #2a334d;padding:2px 6px;border-radius:4px;color:#b48ead}
.last-update{font-size:10px;color:#616e88;text-align:right;margin-top:6px}
.placeholder{grid-column:1/-1;background:#141a2b;border:1px dashed #2a334d;border-radius:10px;padding:32px;text-align:center;color:#81a1c1;font-size:14px;line-height:1.7}
.placeholder b{color:#88c0d0}

.modal{position:fixed;inset:0;background:rgba(0,0,0,.6);display:none;align-items:flex-start;justify-content:center;padding:18px;overflow-y:auto;z-index:100}
.modal.show{display:flex}
.modal .box{background:#141a2b;border:1px solid #2a334d;border-radius:12px;padding:20px;max-width:720px;width:100%;box-shadow:0 8px 32px rgba(0,0,0,.5);margin:40px 0}
.modal h3{color:#88c0d0;margin-bottom:14px;font-size:17px}
.modal .toolbar{display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap;margin-bottom:14px}
.modal .toolbar .right{display:flex;gap:8px;flex-wrap:wrap}
.modal label{display:block;font-size:11px;color:#81a1c1;text-transform:uppercase;letter-spacing:.3px;margin:9px 0 5px}
.modal input,.modal select{width:100%;background:#0e1320;border:1px solid #252d46;color:#eceff4;padding:8px 10px;border-radius:6px;font-size:13px;outline:none}
.modal input:focus,.modal select:focus{border-color:#88c0d0}
.modal .grid2{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.miner-list{margin-top:14px;border-top:1px solid #2a334d;padding-top:12px}
.miner-list .item{background:#0e1320;border:1px solid #252d46;border-radius:8px;padding:10px 12px;margin-bottom:8px;display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap}
.miner-list .item .mname{color:#e5e9f0;font-weight:600;font-size:14px}
.miner-list .item .mdesc{color:#81a1c1;font-size:12px;margin-top:2px}
.scan-status{font-size:12px;color:#88c0d0;margin:10px 0}
.scan-list{max-height:260px;overflow-y:auto;background:#0e1320;border:1px solid #252d46;border-radius:7px;padding:4px;margin:8px 0}
.scan-list .si{padding:7px 10px;border-bottom:1px solid #1c2238;display:flex;justify-content:space-between;align-items:center;gap:10px}
.scan-list .si:last-child{border-bottom:none}
.scan-list .si .nm{color:#e5e9f0;font-size:13px;font-weight:600}
.scan-list .si .ds{color:#81a1c1;font-size:11px}
.ota{max-width:1400px;margin:20px auto 0;padding:14px 18px;background:#141a2b;border:1px solid #2a334d;border-radius:10px;display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap}
.ota span{font-size:13px;color:#81a1c1}
.ota a{color:#88c0d0;font-weight:600;text-decoration:none}
.ota a:hover{text-decoration:underline}
#hb{color:#8fbcbb;margin-left:6px}
#hbts.off{color:#ff6b6b;animation:none}
#hbts.ok{color:#50fa7b}
@keyframes beat{0%,100%{transform:scale(1)}30%{transform:scale(1.2)}60%{transform:scale(.9)}}
#hb{display:inline-block;animation:beat 2s infinite}
@media(max-width:560px){.metrics{grid-template-columns:repeat(2,1fr)}}
</style></head><body>
<header class="hdr">
  <h1>⛏️ ESP32-C6 · Miner Control Panel</h1>
  <div class="meta">
    <span>ESP: <b id="espIp">-</b></span>
    <span>Wi-Fi: <b id="espRssi">-</b> dBm</span>
    <span>Uptime: <b id="espUptime">-</b></span>
    <span>Майнеров: <b id="cnt">-</b>/8</span>
    <span id="hb">⚡ <small id="hbts">Live</small></span>
  </div>
  <div class="nav">
    <button class="btn primary" onclick="openSettings()">⚙️ Настройки</button>
    <button class="btn ghost"   onclick="openLogs()">📜 Логи</button>
    <a href="/update" class="btn ghost" style="text-decoration:none">⬆️ OTA</a>
  </div>
</header>

<div class="grid" id="cards"></div>

<div class="modal" id="logsModal">
  <div class="box" style="max-width:1100px;width:94vw">
    <div class="toolbar">
      <h3 style="margin:0;color:#88c0d0">📜 Консоль логов ESP32</h3>
      <div class="right" style="display:flex;gap:8px;align-items:center;flex-wrap:wrap">
        <label style="font-size:12px;color:#a7beff;display:flex;align-items:center;gap:4px">
          <input type="checkbox" id="logAutoscroll" checked> Автоскролл вниз
        </label>
        <button class="btn ok" onclick="downloadLogs()">💾 Сохранить как .log</button>
        <button class="btn danger" onclick="clearLogs()">🗑 Очистить</button>
        <button class="btn ghost"  onclick="closeLogs()">✕ Закрыть</button>
      </div>
    </div>
    <pre id="logBox" style="
      margin-top:10px;background:#05090f;color:#50fa7b;border:1px solid #202638;border-radius:8px;
      padding:14px;height:62vh;overflow-y:auto;overflow-x:auto;
      font-family:Consolas,'Courier New',monospace;font-size:12px;line-height:1.45;
      white-space:pre-wrap;word-break:break-all;">⏳ Загрузка...</pre>
    <div style="font-size:11px;color:#616e88;margin-top:6px">
      Кольцевой буфер 8 KB · автообновление каждые 2 секунды · только для залогиненного админа
    </div>
  </div>
</div>

<div class="modal" id="settingsModal">
  <div class="box">
    <h3>⚙️ Настройки майнеров</h3>
    <div class="toolbar">
      <span style="font-size:12px;color:#81a1c1">Сохранённые устройства (до 8): <b id="savedCount">0</b></span>
      <div class="right">
        <button class="btn ok" onclick="startScan()">🔍 Сканировать сеть</button>
        <button class="btn primary" onclick="addBlankMiner()">+ Добавить вручную</button>
        <button class="btn ghost" onclick="closeSettings()">✕ Закрыть</button>
      </div>
    </div>

    <div id="scanStatus" class="scan-status" style="display:none">⏳ Сканирование подсети /24... Найдено устройств: <b id="scanCnt">0</b></div>
    <div id="scanList" class="scan-list" style="display:none"></div>

    <h4 style="color:#81a1c1;font-size:13px;margin-top:18px;margin-bottom:4px">Форма добавления</h4>
    <form onsubmit="event.preventDefault();saveMinerForm();">
      <input type="hidden" id="m_idx" value="-1">
      <div class="grid2">
        <div><label>Имя майнера</label><input id="m_name" placeholder="S19 Подвал №1"></div>
        <div><label>Тип майнера</label>
          <select id="m_type">
            <option value="1">Antminer S19/L3 (stock TCP API 4028)</option>
            <option value="2">Whatsminer M50 (BiXBiT HTTP)</option>
            <option value="3">Antminer L3+/L3++ (stock)</option>
          </select>
        </div>
        <div><label>IP адрес</label><input id="m_ip" placeholder="192.168.1.101" required></div>
        <div><label>Порт API</label><input id="m_port" type="number" value="0" placeholder="0 = default (4028/80)"></div>
        <div><label>Логин</label><input id="m_user" placeholder="root / admin"></div>
        <div><label>Пароль</label><input id="m_pass" type="text" placeholder="root / 123456789"></div>
      </div>
      <div style="margin-top:14px;display:flex;gap:8px;flex-wrap:wrap">
        <button class="btn ok" type="submit">💾 Сохранить</button>
        <button class="btn danger" type="button" id="m_delbtn" style="display:none" onclick="deleteCurrentMiner()">🗑 Удалить этот майнер</button>
        <button class="btn ghost" type="button" onclick="resetMinerForm()">↺ Очистить форму</button>
      </div>
    </form>

    <div class="miner-list" id="minerList"></div>

    <div style="margin-top:18px;display:flex;gap:8px;justify-content:space-between;align-items:center;flex-wrap:wrap;border-top:1px solid #2a334d;padding-top:14px">
      <div style="font-size:12px;color:#81a1c1">⚠️ Все настройки сохраняются в NVS-память ESP и восстанавливаются после отключения питания.</div>
      <button class="btn danger" onclick="resetAll()">↩ Сбросить ВСЕ настройки + WiFi</button>
    </div>
  </div>
</div>

<div class="ota"><span>⚙️ Прошивка: ESP32-C6-N16 Miner Controller v2.1</span>
<span>👉 <a href="/update">OTA обновление прошивки</a></span></div>

<script>
const fmtTh = (v)=> (v===undefined||v===null||v===""||isNaN(v))?"—":Number(v).toFixed(2)+" TH/s";
const fmtW  = (v)=> (v===undefined||v===null||v===""||isNaN(v))?"—":Number(v).toLocaleString("ru-RU")+" W";
const fmtT  = (a,b)=>{if((!a||a===0)&&(!b||b===0))return"—";return (a?a:"?")+"° / "+(b?b:"?")+"°"};
const fmtTime=(s)=>{if(!s||isNaN(s))return"—";s=Math.floor(s);const d=Math.floor(s/86400),h=Math.floor((s%86400)/3600),m=Math.floor((s%3600)/60);return d>0?d+"д "+h+"ч "+m+"м":h>0?h+"ч "+m+"м":m+"м"};
const fmtUptimeMs=(m)=>{if(!m)return"—";return fmtTime(Math.floor(m/1000))};
const tempClass=(el,a,m)=>{const x=Math.max(a||0,m||0);el.classList.remove("ok","warn","temp");if(x>=80){el.classList.add("temp");}else if(x>=65)el.classList.add("warn");else el.classList.add("ok")};
const setStatus=(el,on,txt)=>{el.classList.remove("online","offline","warn");const d='<span class="dot"></span>';if(!on){el.classList.add("offline");el.innerHTML=d+"Оффлайн";}else if(/error|fail|dead/i.test(txt||"")){el.classList.add("warn");el.innerHTML=d+"Ошибка";}else{el.classList.add("online");el.innerHTML=d+"Онлайн";}};
const fmtFans=(a)=>{if(!a||a.length===0)return'<span style="color:#616e88;font-size:11px">нет данных</span>';return a.map((r,i)=>'<span class="fan">F'+(i+1)+': '+r+'</span>').join("")};

function minerCardHTML(m,i){
  const isWM = m.type==2;
  const badge = isWM ? '<span class="badge what">Whatsminer BiXBiT</span>' : '<span class="badge ant">Antminer Stock</span>';
  const t1label = isWM ? "PSU t0 / t2" : "Chip avg / max";
  const t2label = isWM ? "Env / PSU t1" : "Board temp";
  const h5label = isWM ? "Хешрейт" : "Хешрейт 5s";
  const halabel = isWM ? "Заводской" : "Хешрейт avg";
  const extraWM = isWM ? `
    <div class="row"><b>Модель:</b><span id="mdl_${i}">-</span></div>
    <div class="row"><b>PSU:</b><span id="psu_${i}">-</span></div>` : "";
  return `<div class="card" id="m_${i}">
  <div class="title">
    <h2>${m.name||("Майнер #"+(i+1))} <small>${m.ip}</small> ${badge}</h2>
    <span class="status offline" id="st_${i}"><span class="dot"></span>Оффлайн</span>
  </div>
  <div class="err" id="er_${i}"></div>
  <div class="metrics">
    <div class="metric hash"><div class="label">${h5label}</div><div class="value" id="h5_${i}">-</div></div>
    <div class="metric hash"><div class="label">${halabel}</div><div class="value" id="ha_${i}">-</div></div>
    <div class="metric pwr"><div class="label">Мощность</div><div class="value" id="pw_${i}">-</div></div>
    <div class="metric temp ok" id="tb1_${i}"><div class="label">${t1label}</div><div class="value" id="ct_${i}">-</div></div>
    <div class="metric temp ok" id="tb2_${i}"><div class="label">${t2label}</div><div class="value sm" id="bt_${i}">-</div></div>
    <div class="metric"><div class="label">Shares A / R</div><div class="value sm" id="sh_${i}">-</div></div>
  </div>
  <div class="sub">
    <div class="row"><b>Статус:</b><span id="ts_${i}">-</span></div>
    <div class="row"><b>Uptime:</b><span id="up_${i}">-</span></div>
    <div class="row"><b>Pool 1:</b><span id="p1_${i}">-</span></div>
    <div class="row"><b>Pool 2:</b><span id="p2_${i}">-</span></div>
    ${extraWM}
  </div>
  <div class="row"><b>Fans (RPM):</b><div class="fans" id="fn_${i}"></div></div>
  <div class="reboot">
    <button class="btn danger" id="rb_${i}" onclick="rebootMiner(${i})">🔄 Перезагрузить</button>
    <div class="reboot-info" id="ri_${i}"></div>
  </div>
  <div class="last-update" id="lu_${i}"></div>
</div>`;
}

let lastData=null;
async function refresh(){
  try{
    const r=await fetch("/api/status",{cache:"no-store"});
    if(r.status===401){location.reload();return}
    const d=await r.json();
    lastData=d;
    document.getElementById("espIp").textContent=d.esp_ip||"-";
    document.getElementById("espRssi").textContent=d.esp_rssi||"-";
    document.getElementById("espUptime").textContent=fmtUptimeMs(d.uptime_ms);
    document.getElementById("cnt").textContent=d.list?d.list.length:0;

    const host=document.getElementById("cards");
    if(!d.list||d.list.length===0){
      host.innerHTML='<div class="placeholder">🔌 <b>Майнеры не настроены</b><br>Нажмите <b>«Настройки»</b> в правом верхнем углу → добавьте устройства вручную или запустите <b>сканирование сети</b>.</div>';
      return;
    }
    host.innerHTML=d.list.map((m,i)=>minerCardHTML(m,i)).join("");
    const espUptimeMs = d.uptime_ms || 0;
    d.list.forEach((s,i)=>{
      setStatus(document.getElementById("st_"+i),s.online,s.status);
      const er=document.getElementById("er_"+i);
      if(s.error){er.textContent="⚠️ "+s.error;er.classList.add("show");}else er.classList.remove("show");

      // Хешрейты
      if (s.is_whatsminer) {
        document.getElementById("h5_"+i).textContent = fmtTh(s.hashrate_avg_th);
        document.getElementById("ha_"+i).textContent = s.factory_ths ? fmtTh(s.factory_ths) : "—";
      } else {
        document.getElementById("h5_"+i).textContent = fmtTh(s.hashrate_5s_th);
        document.getElementById("ha_"+i).textContent = fmtTh(s.hashrate_avg_th);
      }
      document.getElementById("pw_"+i).textContent = fmtW(s.power_w);

      if (s.is_whatsminer) {
        // PSU temps and env temp
        const p0 = s.psu_temp0!=null?s.psu_temp0:"?";
        const p2 = s.psu_temp2!=null?s.psu_temp2:"?";
        const p1 = s.psu_temp1!=null?s.psu_temp1:"?";
        const env = s.env_temp!=null?s.env_temp:"?";
        document.getElementById("ct_"+i).textContent = p0+"° / "+p2+"°";
        document.getElementById("bt_"+i).textContent = env+"° / "+p1+"°";
        const el1 = document.getElementById("tb1_"+i);
        el1.classList.remove("ok","warn","temp");
        const psuMax = Math.max(s.psu_temp1||0, s.psu_temp2||0);
        if (psuMax >= 95) el1.classList.add("temp");
        else if (psuMax >= 80) el1.classList.add("warn");
        else el1.classList.add("ok");
        const el2 = document.getElementById("tb2_"+i);
        el2.classList.remove("ok","warn","temp");
        el2.classList.add("ok");

        // Model
        const mdl = document.getElementById("mdl_"+i);
        if (mdl) {
          const model = s.miner_model || "?";
          const fact  = s.factory_ths ? " · "+Number(s.factory_ths).toFixed(1)+" TH/s паспорт" : "";
          mdl.textContent = model + fact;
        }
        // PSU V/A
        const psuEl = document.getElementById("psu_"+i);
        if (psuEl) {
          const parts = [];
          if (s.psu_vout) parts.push("вых "+Number(s.psu_vout).toFixed(2)+"V");
          if (s.psu_iout) parts.push(Number(s.psu_iout).toFixed(1)+"A");
          if (s.psu_vin)  parts.push("вход "+Number(s.psu_vin).toFixed(0)+"V");
          if (s.psu_iin)  parts.push(Number(s.psu_iin).toFixed(2)+"A");
          psuEl.textContent = parts.length ? parts.join(" / ") : "—";
        }
      } else {
        document.getElementById("ct_"+i).textContent=fmtT(s.chip_temp_avg,s.chip_temp_max);
        document.getElementById("bt_"+i).textContent=(s.board_temp?s.board_temp+"°C":"—");
        tempClass(document.getElementById("tb1_"+i),s.chip_temp_avg,s.chip_temp_max);
        tempClass(document.getElementById("tb2_"+i),s.board_temp,s.board_temp);
      }

      document.getElementById("sh_"+i).textContent=(s.accepted||0)+" / "+(s.rejected||0)+" ("+Number(s.rejected_rate_pct||0).toFixed(2)+"%)";
      document.getElementById("ts_"+i).textContent=s.status||"-";
      document.getElementById("up_"+i).textContent=fmtTime(s.uptime);
      document.getElementById("p1_"+i).textContent=s.pool1_user||"—";
      document.getElementById("p2_"+i).textContent=s.pool2_user||"—";

      // Fans (+ % для Whatsminer)
      let fansHtml = fmtFans(s.fans);
      if (s.is_whatsminer && s.fan_percent) {
        fansHtml = '<span class="fan" style="color:#50fa7b;border-color:rgba(80,250,123,.4)">'+s.fan_percent+'%</span> ' + fansHtml;
      }
      document.getElementById("fn_"+i).innerHTML = fansHtml;

      const age = (espUptimeMs && s.last_update_ms) ? Math.floor((espUptimeMs - s.last_update_ms)/1000) : -1;
      document.getElementById("lu_"+i).textContent = (age>=0) ? ("Обновлено "+age+" сек назад") : "—";
      const rb=document.getElementById("ri_"+i);rb.classList.remove("ok","fail");
      if(s.reboot_pending||s.reboot_result){rb.textContent=s.reboot_result||"Выполняется...";if(/ok|успеш|200|✅/i.test(s.reboot_result||""))rb.classList.add("ok");if(/fail|ошиб|timeout|❌/i.test(s.reboot_result||""))rb.classList.add("fail");}
    });
  }catch(e){console.error(e);}
}

async function rebootMiner(i){
  const nm=lastData&&lastData.list[i]?lastData.list[i].name||("Майнер #"+(i+1)):"Майнер #"+(i+1);
  if(!confirm("Подтвердите перезагрузку «"+nm+"»?\nМайнер уйдёт в оффлайн на ~60-90 секунд."))return;
  const btn=document.getElementById("rb_"+i);const info=document.getElementById("ri_"+i);
  btn.disabled=true;info.textContent="Отправка команды...";info.classList.remove("ok","fail");
  try{const r=await fetch("/api/reboot/"+i,{method:"POST"});
    const j=await r.json().catch(()=>({ok:false}));
    if(j.ok){info.textContent="✅ Команда принята.";info.classList.add("ok");}
    else{info.textContent="❌ Ошибка (HTTP "+r.status+")";info.classList.add("fail");}}
  catch(e){info.textContent="❌ Сеть: "+e.message;info.classList.add("fail");}
  setTimeout(()=>{btn.disabled=false;},15000);
}

function openSettings(){document.getElementById("settingsModal").classList.add("show");renderMinerList();}
function closeSettings(){document.getElementById("settingsModal").classList.remove("show");}

let s_logTimer = null;
function openLogs(){
  document.getElementById("logsModal").classList.add("show");
  if (!s_logTimer) { refreshLogs(); s_logTimer = setInterval(refreshLogs, 2000); }
}
function closeLogs(){
  document.getElementById("logsModal").classList.remove("show");
  if (s_logTimer) { clearInterval(s_logTimer); s_logTimer = null; }
}
async function refreshLogs(){
  try {
    const r = await fetch("/api/logs",{cache:"no-store"});
    if (!r.ok) return;
    const t = await r.text();
    const box = document.getElementById("logBox");
    if (!box) return;
    box.textContent = t || "(пусто)";
    if (document.getElementById("logAutoscroll").checked) {
      box.scrollTop = box.scrollHeight;
    }
  } catch(e){}
}
async function clearLogs(){ try { await fetch("/api/logs/clear",{method:"POST"}); refreshLogs(); } catch(e){} }
function downloadLogs(){ window.location.href = "/api/logs?download=1&_=" + Date.now(); }

function resetMinerForm(){
  ["m_idx","m_name","m_ip","m_user","m_pass"].forEach(id=>document.getElementById(id).value=id=="m_idx"?"-1":"");
  document.getElementById("m_type").value="1";
  document.getElementById("m_port").value="0";
  document.getElementById("m_delbtn").style.display="none";
}
function fillMinerForm(cfgItem,idx){
  document.getElementById("m_idx").value=idx;
  document.getElementById("m_name").value=cfgItem.name||"";
  document.getElementById("m_type").value=cfgItem.type||1;
  document.getElementById("m_ip").value=cfgItem.ip||"";
  document.getElementById("m_port").value=cfgItem.port||0;
  document.getElementById("m_user").value=cfgItem.user||"";
  document.getElementById("m_pass").value=cfgItem.pass||"";
  document.getElementById("m_delbtn").style.display="inline-block";
}
function addBlankMiner(){resetMinerForm();document.getElementById("m_name").focus();}
async function saveMinerForm(){
  const d={
    idx:parseInt(document.getElementById("m_idx").value),
    name:document.getElementById("m_name").value.trim(),
    type:parseInt(document.getElementById("m_type").value),
    ip:document.getElementById("m_ip").value.trim(),
    port:parseInt(document.getElementById("m_port").value||"0"),
    user:document.getElementById("m_user").value,
    pass:document.getElementById("m_pass").value
  };
  if(!d.ip){alert("IP адрес обязателен");return}
  const r=await fetch("/api/miner/save",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(d)});
  if(r.ok){resetMinerForm();renderMinerList();refresh();}
  else alert("Ошибка сохранения: HTTP "+r.status);
}
async function deleteCurrentMiner(){
  const i=parseInt(document.getElementById("m_idx").value);
  if(i<0)return;
  if(!confirm("Удалить этот майнер из списка?"))return;
  const r=await fetch("/api/miner/delete/"+i,{method:"POST"});
  if(r.ok){resetMinerForm();renderMinerList();refresh();}
  else alert("Ошибка удаления");
}
async function renderMinerList(){
  const r=await fetch("/api/config",{cache:"no-store"});
  const d=await r.json();
  document.getElementById("savedCount").textContent=d.list?d.list.length:0;
  const host=document.getElementById("minerList");host.innerHTML="";
  if(!d.list||d.list.length===0){host.innerHTML='<div style="color:#616e88;font-size:12px;padding:4px">Сохранённых майнеров пока нет.</div>';return;}
  d.list.forEach(m=>{
    const e=document.createElement("div");e.className="item";
    const tname=m.type==2?"Whatsminer BiXBiT":m.type==3?"Antminer L3":"Antminer S19";
    e.innerHTML=`<div><div class="mname">${m.name||("Майнер #"+(m.idx+1))}</div><div class="mdesc">${tname} · ${m.ip}:${m.port||"default"} · ${m.user||"(по умолчанию)"}</div></div>
      <button class="btn ghost" onclick="editMiner(${m.idx})">✎ Редактировать</button>`;
    host.appendChild(e);
  });
}
async function editMiner(idx){
  const r=await fetch("/api/config",{cache:"no-store"});
  const d=await r.json();
  const it=d.list.find(x=>x.idx===idx);if(!it)return;
  fillMinerForm(it,idx);
  window.scrollTo({top:0,behavior:"smooth"});
}
async function startScan(){
  const s=document.getElementById("scanStatus");const l=document.getElementById("scanList");
  const c=document.getElementById("scanCnt");
  s.style.display="block";l.style.display="block";l.innerHTML="<div style='padding:12px;color:#88c0d0;font-size:12px'>Сканирование может занять ~2 минуты...</div>";c.textContent="0";
  await fetch("/api/scan/start");
  const timer=setInterval(async()=>{
    const r=await fetch("/api/scan/state");
    const d=await r.json();
    c.textContent=d.count||0;
    if(!d.running){
      clearInterval(timer);
      s.style.display="none";
      l.innerHTML="";
      if(!d.results||d.results.length===0){l.innerHTML='<div style="padding:12px;color:#616e88;font-size:12px">В подсети не найдено устройств Antminer/Whatsminer. Добавьте вручную.</div>';return;}
      d.results.forEach(n=>{
        const e=document.createElement("div");e.className="si";
        e.innerHTML=`<div><div class="nm">${n.name}</div><div class="ds">${n.ip} · порт ${n.port} · тип ${n.type==2?"Whatsminer BiXBiT":"Antminer"}</div></div>
          <button class="btn ok">Добавить</button>`;
        e.querySelector("button").onclick=()=>{
          resetMinerForm();
          document.getElementById("m_type").value=n.type;
          document.getElementById("m_ip").value=n.ip;
          document.getElementById("m_port").value=n.port;
          document.getElementById("m_name").value=n.name;
          window.scrollTo({top:0,behavior:"smooth"});
        };
        l.appendChild(e);
      });
    }
  },1500);
}
async function resetAll(){
  if(!confirm("Это удалит ВСЕ настройки (майнеры, Wi-Fi, пароль админки) и перезагрузит ESP в режим точки доступа Setup. Продолжить?"))return;
  const r=await fetch("/api/reset",{method:"POST"});
  if(r.ok){document.body.innerHTML='<div style="color:#50fa7b;font-family:monospace;padding:40px;text-align:center">Готово. ESP перезагружается в режим Setup...<br>Подключитесь к <b>MinerControl-Setup</b> через 20 секунд.</div>';setTimeout(()=>location.href="http://192.168.4.1/",4000);}
}
let __tick=0;
async function refreshTick(){
  try{
    await refresh();
    const h=document.getElementById("hbts");
    if(h){h.classList.remove("off");h.classList.add("ok");h.textContent="Live · "+(++__tick);}
  }catch(e){
    console.error(e);
    const h=document.getElementById("hbts");
    if(h){h.classList.remove("ok");h.classList.add("off");h.textContent="Connection lost";}
  }
  setTimeout(refreshTick,5000);
}
refreshTick();
</script></body></html>
)rawliteral";
