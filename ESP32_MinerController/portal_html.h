/*
 * portal_html.h
 * Страницы Captive Portal (первичная настройка Wi-Fi)
 *
 * PORTAL_HTML     — форма ввода SSID/пароля + сканирование Wi-Fi
 * PORTAL_OK_HTML  — страница «Настройки сохранены, ESP перезагружается»
 */

#pragma once

/* ======================================================================== */
/*  PORTAL_HTML — WiFi Setup Page                                           */
/* ======================================================================== */

const char PORTAL_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="ru"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Miner Control · WiFi Setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0;font-family:-apple-system,Segoe UI,Roboto,Consolas,monospace}
body{background:#0a0e1a;color:#e5e9f0;min-height:100vh;padding:18px;display:flex;align-items:center;justify-content:center}
.box{background:#141a2b;border:1px solid #2a334d;border-radius:12px;padding:22px;max-width:480px;width:100%;box-shadow:0 6px 22px rgba(0,0,0,.4)}
h1{font-size:20px;color:#88c0d0;margin-bottom:6px}
p.sub{color:#81a1c1;font-size:13px;margin-bottom:18px}
label{display:block;font-size:12px;color:#81a1c1;text-transform:uppercase;letter-spacing:.3px;margin:12px 0 6px}
input,select{width:100%;background:#0e1320;border:1px solid #252d46;color:#eceff4;padding:10px 12px;border-radius:7px;font-size:14px;outline:none}
input:focus,select:focus{border-color:#88c0d0}
.row{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.scan-btn{margin-top:14px;display:block;background:rgba(136,192,208,.15);color:#88c0d0;border:1px solid rgba(136,192,208,.35);padding:8px 12px;border-radius:7px;cursor:pointer;width:100%;font-size:13px}
.scan-btn:hover{background:rgba(136,192,208,.25)}
.wifi-list{margin-top:10px;max-height:180px;overflow-y:auto}
.wifi-item{padding:8px 10px;background:#0e1320;border:1px solid #252d46;border-radius:6px;margin-bottom:6px;cursor:pointer;font-size:13px}
.wifi-item:hover{border-color:#50fa7b;color:#50fa7b}
.wifi-item small{color:#616e88;margin-left:6px}
.submit{margin-top:18px;background:rgba(80,250,123,.18);border:1px solid rgba(80,250,123,.45);color:#50fa7b;width:100%;padding:12px;border-radius:7px;font-weight:700;font-size:14px;letter-spacing:.3px;text-transform:uppercase;cursor:pointer}
.submit:hover{background:rgba(80,250,123,.3)}
.check{display:flex;align-items:center;gap:8px;margin-top:14px;font-size:13px;color:#b8c0d0}
.check input{width:auto;accent-color:#88c0d0}
.tiny{font-size:11px;color:#616e88;margin-top:4px}
.info{background:rgba(136,192,208,.1);border:1px solid rgba(136,192,208,.25);color:#88c0d0;padding:8px 10px;border-radius:7px;font-size:12px;margin-top:14px}
</style></head><body>
<div class="box">
<h1>⛏️ Miner Control</h1>
<p class="sub">Первичная настройка Wi-Fi для ESP32-C6-N16</p>

<form action="/savewifi" method="POST">
  <label>Сеть Wi-Fi (SSID)</label>
  <input id="ssid" name="ssid" placeholder="Введите SSID или выберите из списка" required>
  <button type="button" class="scan-btn" id="scanBtn">🔍 Сканировать сети Wi-Fi поблизости</button>
  <div class="wifi-list" id="wifiList"></div>

  <label>Пароль Wi-Fi</label>
  <input name="password" type="password" placeholder="Пароль сети (можно оставить пустым для открытой)">

  <div class="check"><input type="checkbox" id="st" name="static" onchange="document.getElementById('iprow').style.display=this.checked?'grid':'none'">
    <label for="st" style="all:unset;color:#b8c0d0;cursor:pointer">Использовать статический IP</label></div>

  <div class="row" id="iprow" style="display:none;margin-top:8px">
    <div><label>IP адрес</label><input name="ip" value="192.168.1.200"></div>
    <div><label>Шлюз</label><input name="gw" value="192.168.1.1"></div>
    <div><label>Маска</label><input name="sn" value="255.255.255.0"></div>
    <div><label>DNS</label><input name="dns" value="8.8.8.8"></div>
  </div>

  <button class="submit" type="submit">💾 Сохранить и подключиться</button>
  <div class="info">⚠️ После сохранения ESP перезагрузится и подключится к указанной сети. Если подключение не удастся — точка поднимется снова.</div>
</form>
</div>
<script>
document.getElementById("scanBtn").onclick=async()=>{
  const list=document.getElementById("wifiList");
  list.innerHTML='<div style="font-size:12px;color:#88c0d0">Сканирование...</div>';
  const r=await fetch("/scan.json",{cache:"no-store"});
  const d=await r.json();
  list.innerHTML="";
  if(d.length===0){list.innerHTML='<div style="font-size:12px;color:#616e88">Сетей не найдено. Введите SSID вручную.</div>';return;}
  d.sort((a,b)=>b.rssi-a.rssi).forEach(n=>{
    const e=document.createElement("div");
    e.className="wifi-item";
    const sec=n.enc?"🔒":"🔓";
    const bars=n.rssi>-55?"▂▄▆█":n.rssi>-70?"▂▄▆":n.rssi>-80?"▂▄":"▂";
    e.innerHTML=sec+" "+bars+" <b>"+n.ssid+"</b><small>"+n.rssi+" dBm · "+n.ch+"</small>";
    e.onclick=()=>{document.getElementById("ssid").value=n.ssid;list.innerHTML="";};
    list.appendChild(e);
  });
};
</script></body></html>
)rawliteral";

/* ======================================================================== */
/*  PORTAL_OK_HTML — WiFi Saved Confirmation Page                           */
/* ======================================================================== */

const char PORTAL_OK_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="UTF-8"><title>OK</title>
<style>body{background:#0a0e1a;color:#50fa7b;font-family:monospace;display:flex;align-items:center;justify-content:center;min-height:100vh;text-align:center;padding:18px}
.box{max-width:480px;background:#141a2b;border:1px solid #2a334d;padding:28px;border-radius:12px}h1{font-size:20px;margin-bottom:12px}p{color:#81a1c1;font-size:13px;line-height:1.6}</style></head>
<body><div class="box"><h1>✅ Настройки сохранены</h1>
<p>ESP сейчас перезагрузится и подключится к сети.<br>
Через ~20 секунд открывайте <b style="color:#88c0d0">http://miner-control.local/</b><br>
(либо IP который вы указали, или взять новый IP из списка клиентов роутера).</p>
<p style="margin-top:16px">Логин/пароль веб-интерфейса по умолчанию:<br><b>admin</b> / <b>capybara2026</b></p></div></body></html>
)rawliteral";
