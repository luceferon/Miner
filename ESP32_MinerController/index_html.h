/*
 * index_html.h
 * Основной веб-интерфейс (дашборд) — STA режим (v3)
 */

#pragma once

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="ru"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Miner Control · ESP32-C6</title>
<style>
*{box-sizing:border-box;margin:0;padding:0;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Consolas,monospace}
body{background:#0a0e1a;color:#e5e9f0;min-height:100vh;padding:20px}
.hdr{max-width:1400px;margin:0 auto 14px;display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:12px}
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

.summary-bar{max-width:1400px;margin:0 auto 18px;display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:10px}
.sum-card{background:#121726;border:1px solid #252d46;border-radius:9px;padding:10px 14px;display:flex;flex-direction:column;gap:2px}
.sum-card .lbl{font-size:11px;color:#616e88;text-transform:uppercase;letter-spacing:.4px}
.sum-card .val{font-size:19px;font-weight:700;color:#eceff4}
.sum-card .val.cyan{color:#8be9fd}
.sum-card .val.orange{color:#ffb86c}
.sum-card .val.green{color:#50fa7b}

.via-panel{max-width:1400px;margin:0 auto 18px;background:#121726;border:1px solid #252d46;border-radius:9px;padding:14px;box-sizing:border-box}
.via-heading{display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap;margin-bottom:12px}
.via-title{font-size:12px;color:#81a1c1;text-transform:uppercase;letter-spacing:.4px}
#viaRubTotal{font-size:16px;font-weight:700;color:#a3be8c;overflow-wrap:anywhere}
.via-coins{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(190px,100%),1fr));gap:10px;color:#8be9fd}
.via-coin{min-width:0;padding:10px 12px;background:#0d1220;border:1px solid #252d46;border-radius:7px}
.via-native{font-size:16px;font-weight:700;overflow-wrap:anywhere;font-variant-numeric:tabular-nums}
.via-rub{font-size:13px;color:#a3be8c;margin-top:5px;overflow-wrap:anywhere}
.via-footer{display:flex;justify-content:space-between;gap:6px 16px;flex-wrap:wrap;margin-top:10px;font-size:11px;color:#81a1c1}

.grid{max-width:1400px;margin:0 auto;display:grid;grid-template-columns:repeat(auto-fit,minmax(540px,1fr));gap:16px}
.card{background:#141a2b;border:1px solid #2a334d;border-radius:10px;padding:18px;box-shadow:0 4px 18px rgba(0,0,0,.3)}
.card .title{display:flex;align-items:center;justify-content:space-between;margin-bottom:14px;flex-wrap:wrap;gap:8px}
.card h2{font-size:16px;color:#e5e9f0;font-weight:700;display:flex;align-items:center;flex-wrap:wrap;gap:6px}
.card h2 small{font-size:11px;color:#616e88;font-weight:400}
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
.metric .label{font-size:11px;color:#616e88;text-transform:uppercase;letter-spacing:.4px;display:flex;justify-content:space-between}
.metric .value{font-size:17px;font-weight:700;color:#eceff4;margin-top:3px}
.metric .subval{font-size:11px;color:#81a1c1;margin-top:2px;font-weight:400}
.metric.hash .value{color:#8be9fd}
.metric.pwr .value{color:#ffb86c}
.metric.temp .value{color:#ff5555}
.metric.temp.ok .value{color:#50fa7b}
.metric.temp.warn .value{color:#f1fa8c}

.chains-box{background:#0e1320;border:1px solid #252d46;border-radius:8px;padding:8px 11px;margin-bottom:10px;display:none}
.chains-box.show{display:block}
.chains-title{font-size:11px;color:#616e88;text-transform:uppercase;letter-spacing:.4px;margin-bottom:6px;display:flex;justify-content:space-between}
.chain-row{display:grid;grid-template-columns:56px 1fr 1fr 1.2fr auto;gap:8px;align-items:center;font-size:12px;padding:4px 0;border-top:1px solid #182035}
.chain-row:first-of-type{border-top:none}
.chain-badge{font-weight:700;color:#88c0d0}
.chain-ok{color:#50fa7b}
.chain-warn{color:#f1fa8c}
.chain-bad{color:#ff6b6b}

.sub{display:grid;grid-template-columns:1fr 1fr;gap:9px;margin-bottom:10px}
.row{background:#0e1320;border:1px solid #252d46;border-radius:8px;padding:7px 11px;font-size:12px;color:#b8c0d0;line-height:1.4}
.row.full{grid-column:1/-1}
.row b{color:#e5e9f0;margin-right:6px}

.pools-box{background:#0e1320;border:1px solid #252d46;border-radius:8px;padding:8px 11px;margin-bottom:10px}
.pool-item{display:flex;align-items:center;justify-content:space-between;gap:8px;font-size:12px;padding:3px 0;border-top:1px solid #182035;flex-wrap:wrap}
.pool-item:first-child{border-top:none}
.pool-left{display:flex;align-items:center;gap:6px;min-width:0}
.pool-url{color:#e5e9f0;font-weight:600;word-break:break-all}
.pool-user{color:#8be9fd}
.pool-meta{font-size:11px;color:#81a1c1}
.pbadge{font-size:10px;padding:1px 6px;border-radius:4px;font-weight:700;text-transform:uppercase}
.pbadge.active{background:rgba(80,250,123,.18);color:#50fa7b;border:1px solid rgba(80,250,123,.4)}
.pbadge.alive{background:rgba(136,192,208,.12);color:#88c0d0;border:1px solid rgba(136,192,208,.3)}
.pbadge.dead{background:rgba(255,85,85,.15);color:#ff6b6b;border:1px solid rgba(255,85,85,.35)}

.err{background:rgba(255,85,85,.1);border:1px solid rgba(255,85,85,.3);color:#ff6b6b;padding:9px 11px;border-radius:8px;font-size:12px;margin-bottom:11px;display:none;line-height:1.4}
.err.show{display:block}
.reboot{display:flex;align-items:center;justify-content:space-between;gap:12px;padding-top:11px;border-top:1px dashed #2a334d;flex-wrap:wrap;margin-top:8px}
.reboot-info{font-size:12px;color:#81a1c1;flex:1;min-width:220px}
.reboot-info.ok{color:#50fa7b}
.reboot-info.fail{color:#ff6b6b}
.fans{display:flex;gap:5px;flex-wrap:wrap;margin-top:3px}
.fan{font-size:11px;background:#1b2238;border:1px solid #2a334d;padding:2px 7px;border-radius:4px;color:#b48ead}
.fan.bad{background:rgba(255,85,85,.15);border-color:#ff5555;color:#ff6b6b;font-weight:700}
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
@media(max-width:600px){
  .metrics{grid-template-columns:repeat(2,1fr)}
  .grid{grid-template-columns:1fr}
  .chain-row{grid-template-columns:45px 1fr 1fr;row-gap:3px}
}
</style></head><body>
<header class="hdr">
  <h1>⛏️ ESP32-C6 · Miner Control Panel <small style="font-size:12px;color:#616e88">v3</small></h1>
  <div class="meta">
    <span>ESP: <b id="espIp">-</b></span>
    <span>Wi-Fi: <b id="espRssi">-</b> dBm</span>
    <span>Uptime: <b id="espUptime">-</b></span>
    <span>Устройств: <b id="cnt">-</b>/8</span>
    <span id="hb">⚡ <small id="hbts">Live</small></span>
  </div>
  <div class="nav">
    <button class="btn primary" onclick="openSettings()">⚙️ Настройки</button>
    <button class="btn ghost"   onclick="openLogs()">📜 Логи</button>
    <a href="/update" class="btn ghost" style="text-decoration:none">⬆️ OTA</a>
  </div>
</header>

<section class="summary-bar" id="summaryBar" style="display:none">
  <div class="sum-card"><span class="lbl">Общий хешрейт</span><span class="val cyan" id="sumHash">0.00 TH/s</span></div>
  <div class="sum-card"><span class="lbl">Общая мощность</span><span class="val orange" id="sumPower">0 W</span></div>
  <div class="sum-card"><span class="lbl">Энергоэффективность</span><span class="val" id="sumEff">— J/TH</span></div>
  <div class="sum-card"><span class="lbl">В сети (Online)</span><span class="val green" id="sumOnline">0 / 0</span></div>
  <div class="sum-card"><span class="lbl">Макс. темп. чипов/БП</span><span class="val" id="sumMaxTemp">—</span></div>

</section>

<section class="via-panel" id="viaCard" style="display:none" aria-label="Баланс ViaBTC">
  <div class="via-heading"><span class="via-title">Баланс ViaBTC</span><span id="viaRubTotal"></span></div>
  <div class="via-coins" id="viaBalance">—</div>
  <div class="via-footer"><span id="viaStatus"></span><span id="viaRubStatus"></span></div>
</section>

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
    <div style="display:flex;gap:12px;align-items:center;flex-wrap:wrap;margin-top:10px">
      <label style="font-size:12px">Записывать ответы:
        <select id="logMinerFilter" onchange="changeLogFilter()" style="width:auto;max-width:360px">
          <option value="-1">Все майнеры</option>
        </select>
      </label>
      <label style="font-size:12px"><input id="logAutoRefresh" type="checkbox" checked style="width:auto"> Обновлять журнал</label>
      <span id="logFilterStatus" style="font-size:12px;color:#81a1c1"></span>
    </div>
    <pre id="logBox" style="
      margin-top:10px;background:#05090f;color:#50fa7b;border:1px solid #202638;border-radius:8px;
      padding:14px;height:62vh;overflow-y:auto;overflow-x:auto;
      font-family:Consolas,'Courier New',monospace;font-size:12px;line-height:1.45;
      white-space:pre-wrap;word-break:break-all;">⏳ Загрузка...</pre>
    <div style="font-size:11px;color:#616e88;margin-top:6px">
      Буфер 64 КБ в ОЗУ · обновление каждые 2 секунды · старые записи вытесняются, счётчик overwritten указан в начале.
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

    <h4 style="color:#81a1c1;font-size:13px;margin-top:18px;margin-bottom:4px">Форма добавления / редактирования</h4>
    <form onsubmit="event.preventDefault();saveMinerForm();">
      <input type="hidden" id="m_idx" value="-1">
      <div class="grid2">
        <div><label>Имя майнера</label><input id="m_name" placeholder="S19 Подвал №1"></div>
        <div><label>Тип майнера</label>
          <select id="m_type">
            <option value="1">Antminer S19/T19 (TCP API 4028)</option>
            <option value="2">Whatsminer M30/M50 (TCP 4028 / BiXBiT)</option>
            <option value="3">Antminer L3+/L3++ (TCP API 4028)</option>
          </select>
        </div>
        <div><label>IP адрес</label><input id="m_ip" placeholder="192.168.0.133" required></div>
        <div><label>Оценка мощности, Вт (если API не передаёт; 0 = авто)</label><input id="m_power" type="number" min="0" max="100000" step="1" value="0"></div>
        <div><label>Порт API</label><input id="m_port" type="number" value="0" placeholder="0 = авто (4028 / 80)"></div>
        <div><label>Логин</label><input id="m_user" placeholder="root / admin"></div>
        <div><label>Пароль</label><input id="m_pass" type="text" placeholder="root / admin"><label style="display:block;font-size:11px;margin-top:6px"><input type="checkbox" id="m_clearpass" style="width:auto"> Сбросить сохранённый пароль</label></div>
      </div>
      <div style="margin-top:14px;display:flex;gap:8px;flex-wrap:wrap">
        <button class="btn ok" type="submit">💾 Сохранить</button>
        <button class="btn danger" type="button" id="m_delbtn" style="display:none" onclick="deleteCurrentMiner()">🗑 Удалить этот майнер</button>
        <button class="btn ghost" type="button" onclick="resetMinerForm()">↺ Очистить форму</button>
      </div>
    </form>

    <div class="miner-list" id="minerList"></div>
    <section style="margin-top:22px;border-top:1px solid #2a334d;padding-top:16px">
      <h4>Баланс ViaBTC</h4>
      <form onsubmit="event.preventDefault();saveVia();">
        <label><input id="viaEnabled" type="checkbox" style="width:auto"> Показывать баланс в верхней панели</label>
        <div class="grid2">
          <div><label>Аккаунт / субаккаунт ViaBTC</label><input id="viaAccount" maxlength="128" autocomplete="off"></div>
          <div><label>API Key</label><input id="viaKey" type="password" maxlength="256" autocomplete="new-password"></div>
          <div><label>Secret Key (необязательно)</label><input id="viaSecret" type="password" maxlength="256" autocomplete="new-password"></div>
        </div>
        <p style="font-size:12px;color:#81a1c1">Обновление раз в минуту. Укажите точное имя аккаунта, которому принадлежит ключ. Добавьте внешний IP этой сети в IP whitelist ViaBTC. Для чтения баланса Secret Key не требуется и не отправляется. Пустое поле ключа сохраняет прежнее значение.</p>
        <div style="margin:14px 0"><label>Монеты для отображения и общего баланса</label>
          <div id="viaCoinChoices" style="display:grid;grid-template-columns:repeat(auto-fit,minmax(110px,1fr));gap:8px;margin-top:8px"></div>
          <p style="font-size:12px;color:#81a1c1">Снимите флажок, чтобы скрыть монету и исключить её из итога. Новые монеты включаются автоматически. Выбор применяется кнопкой «Сохранить ViaBTC».</p>
        </div>
        <label><input id="viaClear" type="checkbox" style="width:auto"> Удалить сохранённые ключи</label>
        <button id="viaSave" class="btn primary" type="submit" disabled>Сохранить ViaBTC</button>
        <div id="viaMessage" role="status" style="font-size:12px;margin-top:8px"></div>
      </form>
    </section>

    <section style="margin-top:22px;border-top:1px solid #2a334d;padding-top:16px">
      <h4 style="color:#88c0d0;margin-bottom:10px">🔐 Пароль веб-интерфейса</h4>
      <form id="webPasswordForm" onsubmit="event.preventDefault();changeWebPassword();">
        <div class="grid2">
          <div><label>Текущий пароль</label><input id="web_current" type="password" autocomplete="current-password" required></div>
          <div><label>Новый пароль</label><input id="web_new" type="password" autocomplete="new-password" minlength="8" maxlength="64" required></div>
          <div><label>Повтор нового пароля</label><input id="web_confirm" type="password" autocomplete="new-password" minlength="8" maxlength="64" required></div>
        </div>
        <p style="font-size:12px;color:#81a1c1;margin:10px 0">
          8–64 байта UTF-8. Новый пароль применяется также к OTA. После сохранения перезапустится только ESP.
        </p>
        <button id="webPasswordSave" class="btn primary" type="submit">Сменить пароль и перезапустить ESP</button>
        <div id="webPasswordStatus" role="status" style="font-size:13px;margin-top:10px;white-space:pre-wrap"></div>
      </form>
    </section>

    <div style="margin-top:18px;display:flex;gap:8px;justify-content:space-between;align-items:center;flex-wrap:wrap;border-top:1px solid #2a334d;padding-top:14px">
      <div style="font-size:12px;color:#81a1c1">⚠️ Все настройки сохраняются в NVS-память ESP.</div>
      <button class="btn danger" onclick="resetAll()">↩ Сбросить ВСЕ настройки + WiFi</button>
    </div>
  </div>
</div>

<div class="ota"><span>⚙️ Прошивка: ESP32-C6-N16 Miner Controller v3</span>
<span>👉 <a href="/update">OTA обновление прошивки</a></span></div>

<script>
const escapeHTML = value => String(value ?? "").replace(/[&<>"']/g, ch => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[ch]));
async function apiFetch(url, options = {}) {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 15000);
  try {
    const response = await fetch(url, {...options, cache:"no-store", signal:controller.signal});
    if (!response.ok) throw new Error("HTTP " + response.status + ": " + (await response.text()).slice(0,160));
    const body = await response.text();
    return {ok:true, status:response.status, json:async()=>JSON.parse(body), text:async()=>body};
  } finally { clearTimeout(timeout); }
}
window.addEventListener("unhandledrejection", event => {
  event.preventDefault();
  console.error(event.reason);
});

const rebootRequests = new Set();
let refreshPending = null;
let stopped = false;
let renderedCardKeys = "";

const fmtTh = (v)=> (v===undefined||v===null||v===""||isNaN(v)||Number(v)<=0)?"—":Number(v).toFixed(2)+" TH/s";
const fmtW  = (v)=> (v===undefined||v===null||v===""||isNaN(v)||Number(v)<=0)?"—":Number(v).toLocaleString("ru-RU")+" W";
const fmtT  = (a,b)=>{if((!a||a===0)&&(!b||b===0))return"—";if(a&&b&&a!==b)return a+"° / "+b+"°C";return (b||a)+"°C";};
const fmtTime=(s)=>{if(!s||isNaN(s))return"—";s=Math.floor(s);const d=Math.floor(s/86400),h=Math.floor((s%86400)/3600),m=Math.floor((s%3600)/60);return d>0?d+"д "+h+"ч "+m+"м":h>0?h+"ч "+m+"м":m+"м"};
const fmtUptimeMs=(m)=>{if(!m)return"—";return fmtTime(Math.floor(m/1000))};

const tempClass=(el,a,m,warnAt=75,critAt=85)=>{
  const x=Math.max(a||0,m||0);
  el.classList.remove("ok","warn","temp");
  if(x>=critAt) el.classList.add("temp");
  else if(x>=warnAt) el.classList.add("warn");
  else el.classList.add("ok");
};

const setStatus=(el,on,txt,deadPool)=>{
  el.classList.remove("online","offline","warn");
  const d='<span class="dot"></span>';
  if(!on){el.classList.add("offline");el.innerHTML=d+"Оффлайн";}
  else if(/error|fail|dead|fault/i.test(txt||"") || deadPool){el.classList.add("warn");el.innerHTML=d+(deadPool?"Пул недоступен":"Внимание");}
  else{el.classList.add("online");el.innerHTML=d+"Онлайн";}
};

const fmtFans=(a, isWM)=>{
  if(!a||a.length===0)return'<span style="color:#616e88;font-size:11px">нет данных</span>';
  const labelsWM = ["In", "Out", "PSU"];
  return a.map((r,i)=>{
    const rpm = Number(r)||0;
    const isBad = rpm > 9500 || (rpm > 0 && rpm < 1200);
    const lbl = (isWM && labelsWM[i]) ? labelsWM[i] : ("F"+(i+1));
    return `<span class="fan ${isBad?'bad':''}" title="${isBad?'Аномальные обороты вентилятора!':''}">${lbl}: ${escapeHTML(rpm)}${isBad?' ⚠️':''}</span>`;
  }).join("");
};

const shortUrl = (u) => {
  if(!u) return "—";
  return String(u).replace(/^stratum\+(tcp|ssl):\/\//i, "");
};

function minerCardHTML(m,i){
  const isWM = !!m.is_whatsminer || m.type==2;
  const badge = isWM ? '<span class="badge what">WhatsMiner</span>' : '<span class="badge ant">Antminer</span>';
  return `<div class="card" id="m_${i}">
  <div class="title">
    <h2>${escapeHTML(m.name||("Майнер #"+(i+1)))} <small>${escapeHTML(m.ip)}</small> ${badge} <small id="mdl_${i}" style="color:#8be9fd"></small></h2>
    <span class="status offline" id="st_${i}"><span class="dot"></span>Оффлайн</span>
  </div>
  <div class="err" id="er_${i}"></div>
  <div class="metrics">
    <div class="metric hash"><div class="label"><span id="hl1_${i}">Хешрейт (5s)</span><span id="hdiff_${i}"></span></div><div class="value" id="h5_${i}">-</div><div class="subval" id="hsub1_${i}"></div></div>
    <div class="metric hash"><div class="label"><span id="hl2_${i}">Хешрейт (Avg)</span></div><div class="value" id="ha_${i}">-</div><div class="subval" id="hsub2_${i}"></div></div>
    <div class="metric pwr"><div class="label"><span>Мощность</span><span id="eff_${i}"></span></div><div class="value" id="pw_${i}">-</div><div class="subval" id="psub_${i}"></div></div>
    <div class="metric temp ok" id="tb1_${i}"><div class="label"><span id="tl1_${i}">Чипы (Avg/Max)</span></div><div class="value" id="ct_${i}">-</div><div class="subval" id="tsub1_${i}"></div></div>
    <div class="metric temp ok" id="tb2_${i}"><div class="label"><span id="tl2_${i}">Платы (PCB)</span></div><div class="value" id="bt_${i}">-</div><div class="subval" id="tsub2_${i}"></div></div>
    <div class="metric"><div class="label"><span>Шapы (A / R)</span><span id="hw_${i}"></span></div><div class="value" style="font-size:15px" id="sh_${i}">-</div><div class="subval" id="shsub_${i}"></div></div>
  </div>

  <div class="chains-box" id="chbox_${i}">
    <div class="chains-title"><span>Хеш-платы (Hashboard Telemetry)</span><span id="chsum_${i}"></span></div>
    <div id="chlist_${i}"></div>
  </div>

  <div class="pools-box" id="pbox_${i}"></div>

  <div class="sub">
    <div class="row"><b>Статус:</b><span id="ts_${i}">-</span></div>
    <div class="row"><b>Uptime:</b><span id="up_${i}">-</span></div>
    <div class="row full" id="psurow_${i}" style="display:none"><b>Блок питания (PSU):</b><span id="psu_${i}">-</span></div>
  </div>
  <div class="row"><b>Охлаждение (RPM):</b><div class="fans" id="fn_${i}"></div></div>
  <div class="reboot">
    <button class="btn danger" id="rb_${i}" onclick="rebootMiner(${i})">🔄 Перезагрузить</button>
    <div class="reboot-info" id="ri_${i}"></div>
  </div>
  <div class="last-update" id="lu_${i}"></div>
  <div class="last-update" id="bu_${i}"></div>
</div>`;
}

let lastData=null;
function refresh(){
  if (!refreshPending) refreshPending = refreshData().finally(()=>{refreshPending=null;});
  return refreshPending;
}
async function refreshData(){
  try{
    const r=await apiFetch("/api/status",{cache:"no-store"});
    if(r.status===401){location.reload();return}
    const d=await r.json();
    lastData=d;
    renderVia(d.viabtc);
    document.getElementById("espIp").textContent=d.esp_ip||"-";
    document.getElementById("espRssi").textContent=d.esp_rssi||"-";
    document.getElementById("espUptime").textContent=fmtUptimeMs(d.uptime_ms);
    document.getElementById("cnt").textContent=d.list?d.list.length:0;

    const host=document.getElementById("cards");
    const sumBar=document.getElementById("summaryBar");
    if(!d.list||d.list.length===0){
      renderedCardKeys = "";
      sumBar.style.display="none";
      host.innerHTML='<div class="placeholder">🔌 <b>Майнеры не настроены</b><br>Нажмите <b>«Настройки»</b> в правом верхнем углу → добавьте устройства вручную или запустите <b>сканирование сети</b>.</div>';
      return;
    }

    sumBar.style.display="grid";
    const newKey = d.list.map(m => `${m.idx}:${m.type}:${m.is_whatsminer?1:0}:${m.name}:${m.ip}`).join("|");
    if (newKey !== renderedCardKeys) {
      host.innerHTML = d.list.map(m=>minerCardHTML(m,m.idx)).join("");
      renderedCardKeys = newKey;
    }

    let totalHash = 0, totalPower = 0, onlineCnt = 0, globalMaxTemp = 0;
    let effWeightedHash = 0, effWeightedPower = 0;
    let hasEstimatedPower = false, hasUnknownPower = false;
    const espUptimeMs = d.uptime_ms || 0;

    d.list.forEach(s=>{
      const i = s.idx;
      const isWM = !!s.is_whatsminer;
      const curHash = Number(isWM ? s.hashrate_avg_th : (s.hashrate_5s_th || s.hashrate_avg_th)) || 0;
      const curPower = Number(s.power_w) || 0;
      if (s.online) {
        onlineCnt++;
        totalHash += curHash;
        totalPower += curPower;
        hasEstimatedPower ||= !!s.power_estimated;
        hasUnknownPower ||= !s.power_available;
        if (curHash > 0 && curPower > 0) {
          effWeightedHash += curHash;
          effWeightedPower += curPower;
        }
      }

      // Проверка отвала приоритетного пула
      const p0Dead = s.pools && s.pools.length > 0 && s.pools[0].status === "Dead";
      setStatus(document.getElementById("st_"+i), s.online, s.status, p0Dead);

      const er=document.getElementById("er_"+i);
      let errText = s.error || s.errorMsg || "";
      if (!errText && p0Dead) errText = "Приоритетный Pool 1 (" + shortUrl(s.pools[0].url) + ") в статусе DEAD! Работает резервный пул.";
      if(errText){er.textContent="⚠️ "+errText;er.classList.add("show");}else er.classList.remove("show");

      // Заголовок модели
      const mdlEl = document.getElementById("mdl_"+i);
      if (mdlEl) mdlEl.textContent = s.miner_model ? ("· " + s.miner_model) : "";

      // Хешрейт и КПД
      if (isWM) {
        document.getElementById("hl1_"+i).textContent = "Хешрейт (Avg)";
        document.getElementById("hl2_"+i).textContent = "Паспорт (Factory)";
        document.getElementById("h5_"+i).textContent = fmtTh(s.hashrate_avg_th);
        document.getElementById("ha_"+i).textContent = s.factory_ths ? fmtTh(s.factory_ths) : "—";
        if (s.factory_ths > 0 && s.hashrate_avg_th > 0) {
          const pct = ((s.hashrate_avg_th - s.factory_ths) / s.factory_ths) * 100;
          const sign = pct >= 0 ? "+" : "";
          document.getElementById("hdiff_"+i).textContent = sign + pct.toFixed(1) + "%";
          document.getElementById("hdiff_"+i).style.color = pct >= 0 ? "#50fa7b" : "#ffb86c";
        } else {
          document.getElementById("hdiff_"+i).textContent = "";
        }
        document.getElementById("hsub1_"+i).textContent = s.mem_usage ? ("ОЗУ майнера: " + s.mem_usage + "%") : "";
        document.getElementById("hsub2_"+i).textContent = s.no_fee_penalty ? "⚠️ Штраф No-Fee" : "Режим: Норма";
      } else {
        document.getElementById("hl1_"+i).textContent = "Хешрейт (5s)";
        document.getElementById("hl2_"+i).textContent = "Хешрейт (Avg / 30m)";
        document.getElementById("h5_"+i).textContent = fmtTh(s.hashrate_5s_th);
        document.getElementById("ha_"+i).textContent = fmtTh(s.hashrate_avg_th);
        if (s.ideal_ths > 0 && s.hashrate_avg_th > 0) {
          const pct = ((s.hashrate_avg_th / s.ideal_ths) * 100);
          document.getElementById("hdiff_"+i).textContent = pct.toFixed(1) + "% от ном.";
        } else {
          document.getElementById("hdiff_"+i).textContent = "";
        }
        document.getElementById("hsub1_"+i).textContent = s.ideal_ths ? ("Номинал: " + Number(s.ideal_ths).toFixed(1) + " TH/s") : "";
        document.getElementById("hsub2_"+i).textContent = s.rate_30m_th ? ("30m: " + Number(s.rate_30m_th).toFixed(2) + " TH/s") : "";
      }

      document.getElementById("pw_"+i).textContent = (s.power_available ? (curPower.toLocaleString("ru-RU") + " W") : "—") + (s.power_estimated ? " (оценка)" : "");
      if (curPower > 0 && curHash > 0) {
        const jth = curPower / curHash;
        document.getElementById("eff_"+i).textContent = jth.toFixed(1) + " J/TH" + (s.power_estimated ? " (оценка)" : "");
        document.getElementById("eff_"+i).style.color = "#50fa7b";
      } else {
        document.getElementById("eff_"+i).textContent = "";
      }
      document.getElementById("psub_"+i).textContent = s.freq_avg ? ("Частота: " + s.freq_avg + " MHz") : (s.psu_name ? ("БП: " + s.psu_name) : "Ваттметр не передан в API");

      const boardsAge = s.boards_available ? Math.floor(((espUptimeMs - s.boards_last_update_ms) >>> 0) / 1000) : -1;
      const boardsStale = !!s.boards_stale || boardsAge > 30;
      const boardsLabel = document.getElementById("bu_"+i);
      boardsLabel.textContent = boardsAge < 0 ? "Платы: данные не получены" :
        "Платы: " + (boardsStale ? "⚠ устаревшие данные · " : "") + boardsAge + " сек назад";
      boardsLabel.style.color = (boardsAge < 0 || boardsStale) ? "#f1fa8c" : "";
      // Температуры
      if (isWM && !s.chip_temp_max) {
        document.getElementById("tl1_"+i).textContent = "Датчики БП (t1 / t2)";
        document.getElementById("tl2_"+i).textContent = "Впуск (Env / t0)";
        const p0 = s.psu_temp0!=null?s.psu_temp0:"?";
        const p1 = s.psu_temp1!=null?s.psu_temp1:"?";
        const p2 = s.psu_temp2!=null?s.psu_temp2:"?";
        const env = s.env_temp!=null?s.env_temp:"?";
        document.getElementById("ct_"+i).textContent = p1+"° / "+p2+"°C";
        document.getElementById("bt_"+i).textContent = env+"° / "+p0+"°C";
        document.getElementById("tsub1_"+i).textContent = s.chip_temp_target ? ("Цель чипов: "+s.chip_temp_target+"° (Защита: "+s.chip_temp_protect+"°)") : "";
        document.getElementById("tsub2_"+i).textContent = "Температура воздуха на входе";
        const psuMax = Math.max(s.psu_temp0||0, s.psu_temp1||0, s.psu_temp2||0);
        if (s.online && psuMax > globalMaxTemp) globalMaxTemp = psuMax;
        tempClass(document.getElementById("tb1_"+i), psuMax, psuMax, 75, 90);
        tempClass(document.getElementById("tb2_"+i), s.env_temp||0, s.env_temp||0, 38, 45);
      } else {
        document.getElementById("tl1_"+i).textContent = "Чипы (Avg / Max)";
        document.getElementById("tl2_"+i).textContent = "Платы PCB (Avg / Max)";
        document.getElementById("ct_"+i).textContent = fmtT(s.chip_temp_avg, s.chip_temp_max);
        document.getElementById("bt_"+i).textContent = fmtT(s.board_temp, s.board_temp_max || s.board_temp);
        document.getElementById("tsub1_"+i).textContent = isWM && s.env_temp ? ("Впуск: "+s.env_temp+"°C · БП: "+(s.psu_temp2||0)+"°C") : "Рабочая норма чипов < 80°C";
        document.getElementById("tsub2_"+i).textContent = s.total_acn ? ("Всего активных чипов: " + s.total_acn) : "";
        if (s.online && !boardsStale && (s.chip_temp_max||0) > globalMaxTemp) globalMaxTemp = s.chip_temp_max;
        tempClass(document.getElementById("tb1_"+i), s.chip_temp_avg, s.chip_temp_max, 75, 85);
        tempClass(document.getElementById("tb2_"+i), s.board_temp, s.board_temp_max||s.board_temp, 65, 75);
      }

      // Шары и ошибки HW
      document.getElementById("sh_"+i).textContent=(s.accepted||0).toLocaleString("ru-RU")+" / "+(s.rejected||0);
      document.getElementById("hw_"+i).textContent = s.hw_errors ? ("HW: "+s.hw_errors) : "";
      document.getElementById("shsub_"+i).textContent = "Rej: "+Number(s.rejected_rate_pct||0).toFixed(2)+"%" + (s.stale_shares ? (" · Stale: "+s.stale_shares) : "");

      // Детализация хеш-плат (Chains)
      const chBox = document.getElementById("chbox_"+i);
      const chList = document.getElementById("chlist_"+i);
      const chSum = document.getElementById("chsum_"+i);
      if (s.chains && s.chains.length > 0) {
        chBox.classList.add("show");
        chSum.textContent = `Плат: ${s.chains.length} · Чипов: ${s.total_acn || "—"}`;
        chList.innerHTML = s.chains.map(c => {
          const acnCls = c.bad_asic ? "chain-bad" : "chain-ok";
          const rateTh = (Number(c.rate_gh)||0) / 1000;
          return `<div class="chain-row">
            <span class="chain-badge">Плата #${c.id}</span>
            <span><b>${rateTh.toFixed(2)}</b> TH/s</span>
            <span class="${acnCls}">${c.acn} чипов ${c.bad_asic?'⚠️':'✓'}</span>
            <span style="color:#81a1c1" title="Градиент температур чипов (вход → выход)">🌡️ ${escapeHTML(c.temp_chip || (c.temp ? c.temp+"°C" : "—"))}</span>
            <span style="color:#616e88;text-align:right">HW: ${c.hw||0}</span>
          </div>`;
        }).join("");
      } else {
        chBox.classList.remove("show");
      }

      // Блок пулов
      const pBox = document.getElementById("pbox_"+i);
      if (s.pools && s.pools.length > 0) {
        pBox.innerHTML = s.pools.map((p, idx) => {
          const stClass = p.active ? "active" : (p.status === "Alive" ? "alive" : "dead");
          const stText  = p.active ? "ACTIVE" : (p.status === "Alive" ? "BACKUP" : "DEAD");
          const extra = [];
          if (p.diff) extra.push("Diff: " + p.diff);
          if (p.accepted) extra.push("A: " + Number(p.accepted).toLocaleString("ru-RU"));
          return `<div class="pool-item">
            <div class="pool-left">
              <span class="pbadge ${stClass}">${stText}</span>
              <span class="pool-url">P${idx+1}: ${escapeHTML(shortUrl(p.url))}</span>
              <span class="pool-user">(${escapeHTML(p.user||"—")})</span>
            </div>
            <div class="pool-meta">${escapeHTML(extra.join(" · "))}</div>
          </div>`;
        }).join("");
      } else {
        pBox.innerHTML = `<div class="pool-item"><span class="pool-meta">Пул 1: ${escapeHTML(s.pool1_user||"—")} · Пул 2: ${escapeHTML(s.pool2_user||"—")}</span></div>`;
      }

      document.getElementById("ts_"+i).textContent=s.status||"-";
      document.getElementById("up_"+i).textContent=fmtTime(s.uptime);

      // PSU row для WhatsMiner
      const psuRow = document.getElementById("psurow_"+i);
      const psuEl = document.getElementById("psu_"+i);
      if (isWM && (s.psu_vout || s.psu_vin)) {
        psuRow.style.display = "block";
        const parts = [];
        if (s.psu_name) parts.push(s.psu_name);
        if (s.psu_vin)  parts.push("Вход: "+Number(s.psu_vin).toFixed(0)+"V / "+Number(s.psu_iin||0).toFixed(1)+"A");
        if (s.psu_vout) parts.push("Выход: "+Number(s.psu_vout).toFixed(2)+"V / "+Number(s.psu_iout||0).toFixed(1)+"A");
        psuEl.textContent = parts.join(" · ");
      } else {
        psuRow.style.display = "none";
      }

      // Fans
      let fansHtml = fmtFans(s.fans, isWM);
      if (isWM && s.fan_percent) {
        fansHtml = '<span class="fan" style="color:#50fa7b;border-color:rgba(80,250,123,.4)">Шим: '+escapeHTML(s.fan_percent)+'%</span> ' + fansHtml;
      }
      document.getElementById("fn_"+i).innerHTML = fansHtml;

      const age = (espUptimeMs && s.last_update_ms) ? Math.floor(((espUptimeMs - s.last_update_ms) >>> 0)/1000) : -1;
      document.getElementById("lu_"+i).textContent = (age>=0) ? ("Обновлено "+age+" сек назад") : "—";
      document.getElementById("rb_"+i).disabled=!!s.reboot_pending || rebootRequests.has(i);
      const rb=document.getElementById("ri_"+i);rb.classList.remove("ok","fail");
      if(s.reboot_pending||s.reboot_result){
        rb.textContent=s.reboot_result||"Выполняется...";
        if(/ok|успеш|200|✅/i.test(s.reboot_result||""))rb.classList.add("ok");
        if(/fail|ошиб|timeout|❌/i.test(s.reboot_result||""))rb.classList.add("fail");
      } else {
        rb.textContent="";
      }
    });

    // Обновление Summary Bar
    document.getElementById("sumHash").textContent = totalHash.toFixed(2) + " TH/s";
    document.getElementById("sumPower").textContent = totalPower > 0 ? (totalPower.toLocaleString("ru-RU") + " W") : "—";
    document.getElementById("sumEff").textContent = (effWeightedHash > 0 && effWeightedPower > 0) ? ((effWeightedPower / effWeightedHash).toFixed(1) + " J/TH") : "— J/TH";
    const powerNote = hasUnknownPower ? " (неполные данные)" : (hasEstimatedPower ? " (оценка)" : "");
    document.getElementById("sumPower").textContent += powerNote;
    document.getElementById("sumEff").textContent += powerNote;
    document.getElementById("sumOnline").textContent = `${onlineCnt} / ${d.list.length}`;
    document.getElementById("sumMaxTemp").textContent = globalMaxTemp > 0 ? (globalMaxTemp + " °C") : "—";
    document.getElementById("sumMaxTemp").style.color = globalMaxTemp >= 85 ? "#ff5555" : (globalMaxTemp >= 75 ? "#f1fa8c" : "#50fa7b");

  }catch(e){console.error(e);throw e;}
}

async function rebootMiner(i){
  const miner=lastData?.list.find(m=>m.idx===i);
  if (!miner || miner.reboot_pending || rebootRequests.has(i)) return;
  const nm=miner.name||("Майнер #"+(i+1));
  if(!confirm("Подтвердите перезагрузку «"+nm+"»?\nМайнер уйдёт в оффлайн на ~60-90 секунд."))return;
  const btn=document.getElementById("rb_"+i);const info=document.getElementById("ri_"+i);
  rebootRequests.add(i);
  btn.disabled=true;info.textContent="Отправка команды...";info.classList.remove("ok","fail");
  try{const r=await apiFetch("/api/reboot/"+i,{method:"POST"});
    const j=await r.json().catch(()=>({ok:false}));
    if(j.ok){info.textContent="✅ Команда принята.";info.classList.add("ok");}
    else{info.textContent="❌ Ошибка (HTTP "+r.status+")";info.classList.add("fail");}}
  catch(e){info.textContent="❌ Сеть: "+e.message;info.classList.add("fail");}
  rebootRequests.delete(i);
  await refresh();
}

let passwordSaving=false;
async function changeWebPassword(){
  if(passwordSaving) return;
  const current=document.getElementById("web_current").value;
  const next=document.getElementById("web_new").value;
  const confirmation=document.getElementById("web_confirm").value;
  const status=document.getElementById("webPasswordStatus");
  const bytes=new TextEncoder().encode(next).length;
  status.style.color="#ffb86c";
  if(!current){status.textContent="Введите текущий пароль.";return;}
  if(bytes<8||bytes>64||/[\u0000-\u001f\u007f]/.test(next)){
    status.textContent="Новый пароль: 8–64 байта UTF-8, без управляющих символов.";return;
  }
  if(next!==confirmation){status.textContent="Новые пароли не совпадают.";return;}
  if(next===current){status.textContent="Новый пароль совпадает с текущим.";return;}
  passwordSaving=true;
  const button=document.getElementById("webPasswordSave");
  button.disabled=true;
  status.textContent="Сохранение...";
  try {
    const response=await apiFetch("/api/web/password",{method:"POST",headers:{"Content-Type":"application/json"},
      body:JSON.stringify({current_password:current,new_password:next,confirm_password:confirmation})});
    const result=await response.json();
    if(!result.ok) throw new Error("Сервер не подтвердил сохранение.");
    stopped=true;
    closeLogs();
    ["web_current","web_new","web_confirm"].forEach(id=>document.getElementById(id).value="");
    status.style.color="#50fa7b";
    status.textContent="Пароль сохранён. ESP перезапускается. Через 15 секунд страница обновится...";
    setTimeout(()=>location.reload(),15000);
  } catch(error) {
    status.textContent="Ошибка: "+error.message;
    passwordSaving=false;
    button.disabled=false;
  }
}

async function loadLogFilter(){
  const select=document.getElementById("logMinerFilter");
  const status=document.getElementById("logFilterStatus");
  try{
    const response=await apiFetch("/api/logs/filter");
    const setting=await response.json();
    const configResponse=await apiFetch("/api/config");
    const config=await configResponse.json();
    select.innerHTML="";
    const all=document.createElement("option");all.value="-1";all.textContent="Все майнеры";select.appendChild(all);
    for(const miner of config.list||[]){
      const option=document.createElement("option");
      option.value=String(miner.idx);
      option.textContent="#"+miner.idx+" · "+(miner.name||"Майнер")+" · "+miner.ip;
      select.appendChild(option);
    }
    select.value=String(setting.idx);
    status.textContent="Фильтр записи действует для всех открытых вкладок.";
  }catch(error){status.textContent="Ошибка фильтра: "+error.message;}
}
async function changeLogFilter(){
  const select=document.getElementById("logMinerFilter"), status=document.getElementById("logFilterStatus");
  select.disabled=true;
  try{
    await apiFetch("/api/logs/filter",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({idx:Number(select.value)})});
    status.textContent="Фильтр применён к новым ответам. Старые записи сохранены.";
  }catch(error){status.textContent="Ошибка фильтра: "+error.message;}
  finally{select.disabled=false;}
}

// Public quotes only: never send account names, balances or ViaBTC credentials.
const viaCoinIds={BTC:"bitcoin",FB:"fractal-bitcoin",NMC:"namecoin",USDT:"tether",LTC:"litecoin",DOGE:"dogecoin",BCH:"bitcoin-cash",XEC:"ecash",DASH:"dash",ZEC:"zcash",ZEN:"zencash",ETC:"ethereum-classic",KAS:"kaspa",CKB:"nervos-network",ETH:"ethereum"};
const rubQuotes={};
let rubAttempt=0,rubBusy=false,rubError="",rubLastVia=null;
const rubFormat=new Intl.NumberFormat("ru-RU",{style:"currency",currency:"RUB",maximumFractionDigits:2});
function rubMoney(n){return n>0&&n<0.01?"< 0,01 ₽":rubFormat.format(n);}
async function updateRubQuotes(v){
  if(!v?.enabled||!v.valid||rubBusy||Date.now()-rubAttempt<300000)return;
  const ids=[...new Set(selectedViaBalances(v).map(x=>viaCoinIds[x.coin]).filter(Boolean))];
  if(!ids.length)return;
  rubBusy=true;rubAttempt=Date.now();
  const ctrl=new AbortController(),timer=setTimeout(()=>ctrl.abort(),12000);
  try{
    const r=await fetch("https://api.coingecko.com/api/v3/simple/price?ids="+encodeURIComponent(ids.join(","))+"&vs_currencies=rub&include_last_updated_at=true",{signal:ctrl.signal,credentials:"omit",referrerPolicy:"no-referrer"});
    if(!r.ok)throw new Error("HTTP "+r.status);
    const d=await r.json();let count=0;
    for(const id of ids){
      const q=d[id];
      if(q&&typeof q.rub==="number"&&Number.isFinite(q.rub)&&q.rub>0&&Number.isFinite(q.last_updated_at)&&q.last_updated_at>0&&q.last_updated_at*1000<=Date.now()+300000){
        rubQuotes[id]={price:q.rub,time:q.last_updated_at*1000};++count;
      }
    }
    if(!count)throw new Error("Нет котировок");
    rubError="";
  }catch(e){rubError="Курсы недоступны"+(e.message.startsWith("HTTP")?": "+e.message:"");}
  finally{clearTimeout(timer);rubBusy=false;if(rubLastVia)renderVia(rubLastVia,false);}
}
function selectedViaBalances(v){
  const excluded=new Set(v?.excluded_coins||[]);
  return (v?.balances||[]).filter(x=>!excluded.has(x.coin));
}
function renderVia(v,requestQuotes=true){
  rubLastVia=v;
  document.getElementById("viaCard").style.display=v?.enabled?"block":"none";
  const value=document.getElementById("viaBalance"), status=document.getElementById("viaStatus");
  let total=0,priced=0,missing=0,old=false;
  const coinCell=(native,rub)=>`<div class="via-coin"><div class="via-native">${escapeHTML(native)}</div><div class="via-rub">${escapeHTML(rub)}</div></div>`;
  const selected=selectedViaBalances(v);
  value.innerHTML=v?.valid?(selected.length?selected.map(x=>{
    const amount=Number(x.amount),q=rubQuotes[viaCoinIds[x.coin]],native=x.amount+" "+x.coin;
    if(!Number.isFinite(amount)||amount<0){++missing;return coinCell(native,"— ₽");}
    if(amount===0){++priced;return coinCell(native,"≈ "+rubMoney(0));}
    if(!q){++missing;return coinCell(native,"Курс недоступен");}
    const stale=Date.now()-q.time>900000;
    old=old||stale;++priced;total+=amount*q.price;
    return coinCell(native,"≈ "+rubMoney(amount*q.price)+(stale?" *":""));
  }).join(""):(v.balances.length?"Все монеты скрыты — выберите их в настройках":"Нет балансов")):"—";
  value.style.color=v?.stale?"#ebcb8b":"";
  status.textContent=!v?.enabled?"":(v.error || (v.stale?"Устаревшие данные":"Баланс обновлён "+v.age_s+" с назад"));
  document.getElementById("viaRubTotal").textContent=v?.valid&&priced?(missing?"Частично: ≈ ":"Всего: ≈ ")+rubMoney(total)+(old||v.stale?" (устарело)":""):"";
  document.getElementById("viaRubStatus").textContent=!v?.enabled?"":[
    "Курсы: CoinGecko · каждые 5 мин",
    old?"* Котировка старше 15 минут":"",
    missing?"Не все монеты оценены":"",rubError
  ].filter(Boolean).join(" · ");
  if(requestQuotes)void updateRubQuotes(v);
}
async function loadVia(){
  const button=document.getElementById("viaSave");button.disabled=true;
  try{
    const r=await apiFetch("/api/viabtc",{cache:"no-store"}), d=await r.json();
    document.getElementById("viaEnabled").checked=d.enabled;
    document.getElementById("viaAccount").value=d.account||"";
    for(const [id,has] of [["viaKey",d.has_api_key],["viaSecret",d.has_secret_key]]){
      const input=document.getElementById(id);input.value="";input.placeholder=has?"Сохранён; оставьте пустым для сохранения":"Не задан";
    }
    const excluded=new Set(d.excluded_coins||[]);
    const coins=[...new Set([...(d.coins||[]),...Object.keys(viaCoinIds),...excluded])];
    const choices=document.getElementById("viaCoinChoices");choices.replaceChildren();
    for(const coin of coins){
      const label=document.createElement("label"),input=document.createElement("input");
      input.type="checkbox";input.value=coin;input.checked=!excluded.has(coin);input.style.width="auto";
      label.append(input,document.createTextNode(" "+coin));choices.append(label);
    }
    document.getElementById("viaClear").checked=false;
    document.getElementById("viaMessage").textContent="";button.disabled=false;
  }catch(e){document.getElementById("viaMessage").textContent=e.message;}
}
async function saveVia(){
  const button=document.getElementById("viaSave");button.disabled=true;
  try{
    await apiFetch("/api/viabtc",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({
      enabled:document.getElementById("viaEnabled").checked,account:document.getElementById("viaAccount").value.trim(),
      api_key:document.getElementById("viaKey").value.trim(),secret_key:document.getElementById("viaSecret").value.trim(),clear_keys:document.getElementById("viaClear").checked,
      excluded_coins:[...document.querySelectorAll("#viaCoinChoices input")].filter(x=>!x.checked).map(x=>x.value)
    })});
    await loadVia();document.getElementById("viaMessage").textContent="Сохранено. Баланс обновится автоматически.";await refresh();
  }catch(e){document.getElementById("viaMessage").textContent=e.message;}
  finally{button.disabled=false;}
}

function openSettings(){document.getElementById("settingsModal").classList.add("show");renderMinerList();loadVia();}
function closeSettings(){document.getElementById("settingsModal").classList.remove("show");}

let s_logTimer = null;
function openLogs(){
  loadLogFilter();
  document.getElementById("logsModal").classList.add("show");
  if (!s_logTimer) { refreshLogs(); s_logTimer = setInterval(refreshLogs, 2000); }
}
function closeLogs(){
  document.getElementById("logsModal").classList.remove("show");
  if (s_logTimer) { clearInterval(s_logTimer); s_logTimer = null; }
}
let logBusy=false;
async function refreshLogs(){
  if(logBusy||stopped||!document.getElementById("logAutoRefresh").checked) return;
  logBusy=true;
  try {
    const r = await apiFetch("/api/logs",{cache:"no-store"});
    if (!r.ok) return;
    const t = await r.text();
    const box = document.getElementById("logBox");
    if (!box) return;
    box.textContent = t || "(пусто)";
    if (document.getElementById("logAutoscroll").checked) {
      box.scrollTop = box.scrollHeight;
    }
  } catch(e){document.getElementById("logBox").textContent="Ошибка чтения журнала: "+e.message;}
  finally {logBusy=false;}
}
async function clearLogs(){
  try {
    await apiFetch("/api/logs/clear",{method:"POST"});
    document.getElementById("logBox").textContent="Журнал очищен.";
    refreshLogs();
  } catch(error){document.getElementById("logFilterStatus").textContent="Ошибка очистки: "+error.message;}
}
function downloadLogs(){ window.location.href = "/api/logs?download=1&_=" + Date.now(); }

function resetMinerForm(){
  ["m_idx","m_name","m_ip","m_user","m_pass"].forEach(id=>document.getElementById(id).value=id=="m_idx"?"-1":"");
  document.getElementById("m_type").value="1";
  document.getElementById("m_port").value="0";
  document.getElementById("m_power").value="0";
  document.getElementById("m_pass").placeholder="Пароль майнера";
  document.getElementById("m_clearpass").checked=false;
  document.getElementById("m_delbtn").style.display="none";
}
function fillMinerForm(cfgItem,idx){
  document.getElementById("m_clearpass").checked=false;
  document.getElementById("m_idx").value=idx;
  document.getElementById("m_name").value=cfgItem.name||"";
  document.getElementById("m_type").value=cfgItem.type||1;
  document.getElementById("m_ip").value=cfgItem.ip||"";
  document.getElementById("m_port").value=cfgItem.port||0;
  document.getElementById("m_power").value=cfgItem.estimated_power_w||0;
  document.getElementById("m_user").value=cfgItem.user||"";
  document.getElementById("m_pass").value="";
  document.getElementById("m_pass").placeholder=cfgItem.has_pass?"Оставьте пустым, чтобы сохранить пароль":"Пароль не задан";
  document.getElementById("m_delbtn").style.display="inline-block";
}
function addBlankMiner(){resetMinerForm();document.getElementById("m_name").focus();}
async function saveMinerForm(){
  const d={
    idx:parseInt(document.getElementById("m_idx").value),
    name:document.getElementById("m_name").value.trim(),
    type:parseInt(document.getElementById("m_type").value),
    ip:document.getElementById("m_ip").value.trim(),
    port:Number(document.getElementById("m_port").value||"0"),
    estimated_power_w:Number(document.getElementById("m_power").value||"0"),
    user:document.getElementById("m_user").value,
    pass:document.getElementById("m_pass").value
  };
  if(!Number.isInteger(d.estimated_power_w)||d.estimated_power_w<0||d.estimated_power_w>100000) {alert("Оценка мощности: целое число 0–100000 Вт");return;}
  const octets=d.ip.split(".");
  if(octets.length!==4 || octets.some(x=>!/^\d{1,3}$/.test(x)||Number(x)>255)) {alert("Введите корректный IPv4");return;}
  if(!Number.isInteger(d.port)||d.port<0||d.port>65535) {alert("Порт: 0–65535");return;}
  if(document.getElementById("m_clearpass").checked) d.pass="";
  else if(d.idx>=0 && !d.pass) delete d.pass;
  const r=await apiFetch("/api/miner/save",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(d)});
  if(r.ok){resetMinerForm();renderMinerList();refresh();}
  else alert("Ошибка сохранения: HTTP "+r.status);
}
async function deleteCurrentMiner(){
  const i=parseInt(document.getElementById("m_idx").value);
  if(i<0)return;
  if(!confirm("Удалить этот майнер из списка?"))return;
  const r=await apiFetch("/api/miner/delete/"+i,{method:"POST"});
  if(r.ok){resetMinerForm();renderMinerList();refresh();}
  else alert("Ошибка удаления");
}
async function renderMinerList(){
  const r=await apiFetch("/api/config",{cache:"no-store"});
  const d=await r.json();
  document.getElementById("savedCount").textContent=d.list?d.list.length:0;
  const host=document.getElementById("minerList");host.innerHTML="";
  if(!d.list||d.list.length===0){host.innerHTML='<div style="color:#616e88;font-size:12px;padding:4px">Сохранённых майнеров пока нет.</div>';return;}
  d.list.forEach(m=>{
    const e=document.createElement("div");e.className="item";
    const tname=m.type==2?"WhatsMiner":m.type==3?"Antminer L3":"Antminer S19";
    e.innerHTML=`<div><div class="mname">${escapeHTML(m.name||("Майнер #"+(m.idx+1)))}</div><div class="mdesc">${tname} · ${escapeHTML(m.ip)}:${m.port||"default"} · ${escapeHTML(m.user||"(по умолчанию)")}</div></div>
      <button class="btn ghost" onclick="editMiner(${m.idx})">✎ Редактировать</button>`;
    host.appendChild(e);
  });
}
async function editMiner(idx){
  const r=await apiFetch("/api/config",{cache:"no-store"});
  const d=await r.json();
  const it=d.list.find(x=>x.idx===idx);if(!it)return;
  fillMinerForm(it,idx);
  window.scrollTo({top:0,behavior:"smooth"});
}
let scanActive=false;
async function startScan(){
  if(scanActive) return;
  scanActive=true;
  try {
  const s=document.getElementById("scanStatus");const l=document.getElementById("scanList");
  const c=document.getElementById("scanCnt");
  s.style.display="block";l.style.display="block";l.innerHTML="<div style='padding:12px;color:#88c0d0;font-size:12px'>Сканирование может занять несколько минут...</div>";c.textContent="0";
  await apiFetch("/api/scan/start",{method:"POST"});
  const pollScan=async()=>{
    if(stopped) {scanActive=false;return;}
    try {
    const r=await apiFetch("/api/scan/state");
    const d=await r.json();
    c.textContent=d.count||0;
    if(!d.running){
      scanActive=false;
      s.style.display="none";
      l.innerHTML="";
      if(!d.results||d.results.length===0){l.innerHTML='<div style="padding:12px;color:#616e88;font-size:12px">В подсети не найдено устройств Antminer/Whatsminer. Добавьте вручную.</div>';return;}
      d.results.forEach(n=>{
        const e=document.createElement("div");e.className="si";
        e.innerHTML=`<div><div class="nm">${escapeHTML(n.name)}</div><div class="ds">${escapeHTML(n.ip)} · порт ${n.port} · тип ${n.type==2?"WhatsMiner":"Antminer"}</div></div>
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
    if(scanActive) setTimeout(pollScan,1500);
    } catch(error) {scanActive=false;s.style.display="none";l.textContent=error.message;}
  };
  setTimeout(pollScan,1500);
  } catch(error) {scanActive=false;document.getElementById("scanStatus").style.display="none";throw error;}
}
async function resetAll(){
  if(!confirm("Это удалит ВСЕ настройки (майнеры, Wi-Fi, пароль админки) и перезагрузит ESP в режим точки доступа Setup. Продолжить?"))return;
  const r=await apiFetch("/api/reset",{method:"POST"});
  if(r.ok){stopped=true;closeLogs();document.body.innerHTML='<div style="color:#50fa7b;font-family:monospace;padding:40px;text-align:center">Готово. ESP перезагружается в режим Setup...<br>Подключитесь к <b>MinerControl-Setup</b> через 20 секунд.</div>';setTimeout(()=>location.href="http://192.168.4.1/",4000);}
}
let __tick=0;
async function refreshTick(){
  if(stopped) return;
  try{
    await refresh();
    const h=document.getElementById("hbts");
    if(h){h.classList.remove("off");h.classList.add("ok");h.textContent="Live · "+(++__tick);}
  }catch(e){
    console.error(e);
    const h=document.getElementById("hbts");
    if(h){h.classList.remove("ok");h.classList.add("off");h.textContent="Connection lost";}
  }
  if(!stopped) setTimeout(refreshTick,5000);
}
refreshTick();
</script></body></html>
)rawliteral";