#pragma once

#include <Arduino.h>

// Kept in firmware, not LittleFS, so the recovery page remains available even
// when a filesystem image is invalid or an update of web assets is required.
static const char WEB_UPDATE_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="pl">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <meta name="theme-color" content="#0f4677">
  <title>Lay-Z-Spa | Aktualizacja</title>
  <style>
    :root{color-scheme:light dark;--bg:#edf3f7;--card:#fff;--text:#17212b;--muted:#586875;--blue:#0f659b;--blue2:#0f4677;--line:#d4dee5;--warn:#8a4b00;--ok:#08752d;--err:#b42318}
    @media(prefers-color-scheme:dark){:root{--bg:#14202a;--card:#202f3b;--text:#eef5f8;--muted:#b8c6ce;--blue:#36a5e5;--blue2:#102f45;--line:#3d5361;--warn:#ffbf66;--ok:#5ddd86;--err:#ff8d86}}
    *{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:16px/1.45 system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}
    header{background:var(--blue2);color:#fff;padding:18px 20px;box-shadow:0 2px 7px #0004}header div{max-width:820px;margin:auto;display:flex;align-items:center;gap:18px}header a{color:#fff;text-decoration:none;font-size:28px;line-height:1}header span{font-size:22px;font-weight:650}
    main{max-width:820px;margin:24px auto;padding:0 16px 32px}.intro,.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:20px;margin:0 0 18px;box-shadow:0 2px 8px #0001}h1{font-size:27px;margin:0 0 10px}h2{font-size:21px;margin:0 0 8px}.muted{color:var(--muted)}.warning{border-left:5px solid var(--warn);padding-left:13px}.status-grid{display:grid;grid-template-columns:minmax(145px,auto) 1fr;gap:7px 16px;margin-top:15px}.status-grid strong{overflow-wrap:anywhere}
    label{display:block;font-weight:650;margin:13px 0 6px}input[type=file]{width:100%;max-width:560px;border:1px solid var(--line);border-radius:7px;background:var(--card);color:var(--text);padding:8px}
    button{margin-top:14px;border:0;border-radius:8px;padding:11px 17px;background:var(--blue);color:#fff;font:inherit;font-weight:700;cursor:pointer}button:hover{filter:brightness(1.1)}button:disabled{cursor:not-allowed;opacity:.55}
    progress{display:none;width:100%;height:18px;margin:15px 0 5px}.result{display:none;margin-top:14px;padding:12px;border-radius:8px;white-space:pre-wrap}.result.info{display:block;background:#1976d21a;color:var(--text)}.result.ok{display:block;background:#16944724;color:var(--ok)}.result.error{display:block;background:#d92d2024;color:var(--err)}code{overflow-wrap:anywhere}
    @media(max-width:520px){header span{font-size:19px}.status-grid{grid-template-columns:1fr}.status-grid strong{margin-bottom:5px}}
  </style>
</head>
<body>
<header><div><a href="/" aria-label="Powrót do panelu">&#8592;</a><span>Aktualizacja Lay-Z-Spa</span></div></header>
<main>
  <section class="intro">
    <h1>Aktualizacja urządzenia</h1>
    <p>Możesz wgrać firmware oraz pełny obraz plików panelu bez otwierania pompy i podłączania USB.</p>
    <div class="status-grid" aria-live="polite">
      <span>Wersja firmware:</span><strong id="version">odczytywanie…</strong>
      <span>Maks. firmware:</span><strong id="firmwareLimit">odczytywanie…</strong>
      <span>Rozmiar obrazu LittleFS:</span><strong id="filesystemSize">odczytywanie…</strong>
    </div>
  </section>

  <section class="card">
    <h2>1. Firmware</h2>
    <p>Wybierz plik <code>firmware.bin</code> utworzony dla tego modelu ESP8266.</p>
    <label for="firmwareFile">Plik firmware</label>
    <input id="firmwareFile" type="file" accept=".bin,application/octet-stream">
    <button id="firmwareButton" type="button" onclick="startUpload('firmware')">Wgraj firmware i uruchom ponownie</button>
  </section>

  <section class="card warning">
    <h2>2. Pliki panelu (LittleFS)</h2>
    <p>Wybierz plik <code>littlefs.bin</code>. Musi mieć dokładnie rozmiar podany wyżej.</p>
    <p><strong>Uwaga:</strong> obraz zastępuje cały system plików, w tym lokalne statystyki, harmonogramy i ustawienia panelu. Dane Wi-Fi, tożsamość SUPLA i konfiguracja pinów zapisane w EEPROM pozostają zachowane. Jeśli wysyłanie zostanie przerwane, ta strona ratunkowa nadal będzie dostępna z firmware i pozwoli ponowić operację.</p>
    <label for="filesystemFile">Obraz LittleFS</label>
    <input id="filesystemFile" type="file" accept=".bin,application/octet-stream">
    <button id="filesystemButton" type="button" onclick="startUpload('filesystem')">Wgraj pliki i uruchom ponownie</button>
  </section>

  <progress id="progress" max="100" value="0"></progress>
  <div id="result" class="result" role="status" aria-live="assertive"></div>
  <p class="muted"><strong>Nie odłączaj zasilania</strong> podczas zapisu ani pierwszego uruchomienia. Korzystaj wyłącznie z zaufanej sieci lokalnej lub VPN — strona działa przez HTTP.</p>
</main>
<script>
let updateInfo=null,restartBaseline=0,restartWentOffline=false;
const $=id=>document.getElementById(id);
function bytes(value){return Number(value).toLocaleString('pl-PL')+' B';}
function show(message,type){const box=$('result');box.textContent=message;box.className='result '+type;}
function setBusy(busy){$('firmwareButton').disabled=busy;$('filesystemButton').disabled=busy;$('firmwareFile').disabled=busy;$('filesystemFile').disabled=busy;}
async function loadStatus(){
  try{
    const response=await fetch('/update/status',{cache:'no-store'});
    if(!response.ok)throw new Error('HTTP '+response.status);
    updateInfo=await response.json();
    $('version').textContent=updateInfo.firmware;
    $('firmwareLimit').textContent=bytes(updateInfo.maxFirmwareBytes);
    $('filesystemSize').textContent=bytes(updateInfo.filesystemBytes);
    return true;
  }catch(error){
    $('version').textContent='brak połączenia';
    return false;
  }
}
function waitForRestart(){
  let attempts=0;
  const timer=setInterval(async()=>{
    attempts++;
    if(attempts>=150){
      clearInterval(timer);setBusy(false);
      show('Nie udało się dokończyć ponownego uruchamiania. Sprawdź adres IP i zasilanie urządzenia.','error');
      return;
    }
    if(await loadStatus()){
      const rebooted=restartWentOffline||Number(updateInfo.uptimeSeconds)+3<restartBaseline;
      if(!rebooted)return;
      if(updateInfo.postFilesystemRestartPending){
        show('Pierwszy restart zakończony. SUPLA odbudowuje konfigurację; urządzenie wykona jeszcze jeden restart porządkowy…','info');
        return;
      }
      clearInterval(timer);setBusy(false);$('progress').style.display='none';
      show('Urządzenie uruchomiło się ponownie. Aktualizacja zakończona.','ok');
    }else{
      restartWentOffline=true;
    }
  },2000);
}
function startUpload(target){
  if(!updateInfo){show('Nie odczytano parametrów pamięci urządzenia. Odśwież stronę.','error');return;}
  const isFs=target==='filesystem';
  const file=$(isFs?'filesystemFile':'firmwareFile').files[0];
  if(!file){show('Najpierw wybierz plik .bin.','error');return;}
  if(!file.name.toLowerCase().endsWith('.bin')){show('Wybrany plik musi mieć rozszerzenie .bin.','error');return;}
  if(isFs&&file.size!==Number(updateInfo.filesystemBytes)){show('Nieprawidłowy rozmiar obrazu LittleFS. Oczekiwano '+bytes(updateInfo.filesystemBytes)+', wybrano '+bytes(file.size)+'.','error');return;}
  if(!isFs&&(file.size<Number(updateInfo.minFirmwareBytes)||file.size>Number(updateInfo.maxFirmwareBytes))){show('Nieprawidłowy rozmiar firmware. Dozwolony zakres: '+bytes(updateInfo.minFirmwareBytes)+'–'+bytes(updateInfo.maxFirmwareBytes)+'.','error');return;}
  const question=isFs?'Obraz LittleFS zastąpi lokalne pliki, statystyki i ustawienia panelu. Kontynuować?':'Wgrać nowy firmware i uruchomić urządzenie ponownie?';
  if(!confirm(question))return;

  restartBaseline=Number(updateInfo.uptimeSeconds)||0;restartWentOffline=false;
  const request=new XMLHttpRequest();
  const form=new FormData();
  form.append('update',file,file.name);
  request.open('POST',isFs?'/update/filesystem':'/update/firmware',true);
  request.setRequestHeader('X-Update-Size',String(file.size));
  request.upload.onprogress=event=>{if(event.lengthComputable)$('progress').value=Math.round(event.loaded*100/event.total);};
  request.onerror=()=>{setBusy(false);show('Połączenie zostało przerwane. Urządzenie uruchomi się ponownie; wróć na tę stronę i ponów aktualizację.','error');};
  request.onload=()=>{
    let data={};try{data=JSON.parse(request.responseText||'{}');}catch(error){}
    if(request.status===200&&data.ok){
      $('progress').value=100;show('Plik zapisany poprawnie. Oczekiwanie na ponowne uruchomienie…','ok');
      setTimeout(waitForRestart,1800);
    }else{
      setBusy(false);show(data.error||('Aktualizacja nie powiodła się (HTTP '+request.status+').'),'error');
    }
  };
  setBusy(true);$('progress').style.display='block';$('progress').value=0;show('Wysyłanie pliku. Nie odłączaj zasilania…','info');request.send(form);
}
loadStatus();
</script>
</body>
</html>)HTML";
