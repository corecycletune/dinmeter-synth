/*
  ======================================================================
  Module : DinMeter Wi-Fi / OTA Maintenance
  Version: v1.8.3
  ======================================================================
*/

#include "WifiMaintenance.h"

#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <Preferences.h>

namespace {

Preferences wifiPrefs;
WebServer server(80);

WifiMaintMode runtimeMode = WifiMaintMode::OFF;

String savedSsid;
String savedPass;

bool updating = false;
int updateProgress = 0;
String statusText = "OFF";

bool webUpdateOk = false;
bool webUpdateReadyToReboot = false;
size_t webExpectedSize = 0;
size_t webBytesWritten = 0;
uint8_t webUpdateError = 0;
String webUpdateErrorText = "";
uint32_t restartAt = 0;
bool routesRegistered = false;

static constexpr const char* PREF_NS = "dmsynth-wifi";
static constexpr const char* AP_SSID = "DinMeter-Setup";
static constexpr const char* HOSTNAME = "dinmeter";
static constexpr uint32_t STA_TIMEOUT_MS = 10000;

String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length() + 16);
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    switch (c) {
      case '&': out += "&amp;";  break;
      case '<': out += "&lt;";   break;
      case '>': out += "&gt;";   break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c; break;
    }
  }
  return out;
}

String pageHeader(const char* title) {
  String h;
  h.reserve(1400);
  h += F("<!doctype html><html><head><meta charset='utf-8'>");
  h += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  h += F("<title>");
  h += title;
  h += F("</title><style>");
  h += F("body{background:#050505;color:#f6b900;font-family:system-ui,Arial;margin:0;padding:20px}");
  h += F(".box{max-width:650px;margin:auto;border:2px solid #f6b900;padding:18px}");
  h += F("h1,h2{margin-top:0}.warn{background:#f6b900;color:#050505;padding:8px;font-weight:700}");
  h += F("input,button{font-size:16px;padding:10px;margin:5px 0;width:100%;box-sizing:border-box}");
  h += F("button{background:#f6b900;color:#050505;border:0;font-weight:700}");
  h += F("button:hover{cursor:pointer}");
  h += F("button:disabled{background:#6d5d1b;color:#222;cursor:wait}");
  h += F("#uploadStatus{margin-top:12px;font-weight:700}");
  h += F("#uploadBarWrap{display:none;margin-top:10px;border:1px solid #f6b900;height:18px}");
  h += F("#uploadBar{width:0;height:100%;background:#5cff79;transition:width .1s linear}");
  h += F(".muted{color:#bbb;font-size:13px}.ok{color:#5cff79}.bad{color:#ff6868}");
  h += F(".verbox{border:1px solid #f6b900;padding:12px;margin:10px 0;background:#0b0b0b}");
  h += F(".verrow{display:flex;justify-content:space-between;gap:12px;margin:4px 0}");
  h += F(".verlabel{color:#bbb}.vervalue{font-weight:700}");
  h += F(".downgrade{color:#ff6868;font-weight:800}.upgrade{color:#5cff79;font-weight:800}.same{color:#f6b900;font-weight:800}");
  h += F("</style></head><body><div class='box'>");
  return h;
}

String pageFooter() {
  return F("</div></body></html>");
}

String rootPage() {
  String h = pageHeader("DinMeter Maintenance");

  h += F("<div class='warn'>DINMETER SYNTH // MAINTENANCE</div><br>");
  h += F("<h2>Status</h2><p>");
  h += htmlEscape(statusText);
  h += F("</p><p>Mode: ");
  h += htmlEscape(wifiMaintModeText());
  h += F("<br>IP: ");
  h += htmlEscape(wifiMaintIp());
  h += F("<br>Firmware: v1.8.3");
  h += F("</p>");

  h += F("<hr><h2>Firmware Update</h2>");
  h += F("<form id='fwForm'>");
  h += F("<input id='fwFile' type='file' name='firmware' accept='.bin' required>");
  h += F("<button id='fwBtn' type='submit' disabled>SELECT FIRMWARE FIRST</button></form>");
  h += F("<div class='verbox'>");
  h += F("<div class='verrow'><span class='verlabel'>CURRENT</span><span id='currentVersion' class='vervalue'>v1.8.3</span></div>");
  h += F("<div class='verrow'><span class='verlabel'>SELECTED</span><span id='selectedVersion' class='vervalue'>--</span></div>");
  h += F("<div class='verrow'><span class='verlabel'>ACTION</span><span id='versionAction' class='vervalue'>SELECT FILE</span></div>");
  h += F("</div>");
  h += F("<div id='uploadStatus' class='muted'>Select DinMeter_Synth_v1.ino.bin. Its embedded version will be read before upload.</div>");
  h += F("<div id='uploadBarWrap'><div id='uploadBar'></div></div>");
  h += F("<p class='muted'>V2 flow: the Din Meter verifies the complete file first. It will NOT reboot until verification succeeds and you press REBOOT & APPLY.</p>");

  h += F("<script>");
  h += F("const f=document.getElementById('fwForm');");
  h += F("const file=document.getElementById('fwFile');");
  h += F("const btn=document.getElementById('fwBtn');");
  h += F("const st=document.getElementById('uploadStatus');");
  h += F("const wrap=document.getElementById('uploadBarWrap');");
  h += F("const bar=document.getElementById('uploadBar');");
  h += F("const selectedVersion=document.getElementById('selectedVersion');");
  h += F("const versionAction=document.getElementById('versionAction');");
  h += F("const CURRENT_VERSION='v1.8.3';");
  h += F("let rebootMode=false;");
  h += F("let detectedVersion='';");
  h += F("let versionRelation='unknown';");

  h += F("function findAscii(bytes,needle){");
  h += F("const n=new TextEncoder().encode(needle);");
  h += F("outer:for(let i=0;i<=bytes.length-n.length;i++){");
  h += F("for(let j=0;j<n.length;j++){if(bytes[i+j]!==n[j])continue outer;}return i;}");
  h += F("return -1;}");
  h += F("}");

  h += F("function readVersionAfter(bytes,start){");
  h += F("let out='';");
  h += F("for(let i=start;i<bytes.length&&out.length<24;i++){");
  h += F("const c=bytes[i];");
  h += F("if((c>=48&&c<=57)||c===46||c===118){out+=String.fromCharCode(c);}else{break;}");
  h += F("}");
  h += F("const m=out.match(/^v\\d+(?:\\.\\d+){1,3}/);");
  h += F("return m?m[0]:'';");
  h += F("}");

  h += F("function detectFirmwareVersion(bytes){");
  h += F("const exact='DINMETER_FW_VERSION=';");
  h += F("let p=findAscii(bytes,exact);");
  h += F("if(p>=0){const v=readVersionAfter(bytes,p+exact.length);if(v)return v;}");
  h += F("const legacy='DIN SYNTH v';");
  h += F("p=findAscii(bytes,legacy);");
  h += F("if(p>=0){const v=readVersionAfter(bytes,p+legacy.length-1);if(v)return v;}");
  h += F("return '';");
  h += F("}");

  h += F("function verParts(v){");
  h += F("const m=v.match(/v(\\d+)\\.(\\d+)\\.(\\d+)/);");
  h += F("return m?[+m[1],+m[2],+m[3]]:null;");
  h += F("}");

  h += F("function compareVersions(a,b){");
  h += F("const x=verParts(a),y=verParts(b);if(!x||!y)return null;");
  h += F("for(let i=0;i<3;i++){if(x[i]<y[i])return -1;if(x[i]>y[i])return 1;}return 0;");
  h += F("}");

  h += F("file.addEventListener('change',async()=>{");
  h += F("rebootMode=false;detectedVersion='';versionRelation='unknown';");
  h += F("selectedVersion.textContent='--';selectedVersion.className='vervalue';");
  h += F("versionAction.textContent='CHECKING...';versionAction.className='vervalue';");
  h += F("btn.disabled=true;btn.textContent='CHECKING VERSION...';");
  h += F("if(!file.files.length){versionAction.textContent='SELECT FILE';btn.textContent='SELECT FIRMWARE FIRST';return;}");
  h += F("const x=file.files[0];");
  h += F("st.textContent='READING VERSION: '+x.name+' ('+x.size.toLocaleString()+' bytes)';st.className='muted';");
  h += F("try{const bytes=new Uint8Array(await x.arrayBuffer());detectedVersion=detectFirmwareVersion(bytes);}catch(err){detectedVersion='';}");
  h += F("if(!detectedVersion){");
  h += F("selectedVersion.textContent='NOT DETECTED';selectedVersion.className='vervalue bad';");
  h += F("versionAction.textContent='UPLOAD BLOCKED';versionAction.className='vervalue bad';");
  h += F("st.textContent='VERSION NOT FOUND IN THIS .BIN. Upload blocked to prevent selecting the wrong firmware.';st.className='bad';");
  h += F("btn.disabled=true;btn.textContent='VERSION UNKNOWN';return;");
  h += F("}");
  h += F("selectedVersion.textContent=detectedVersion;selectedVersion.className='vervalue';");
  h += F("const cmp=compareVersions(detectedVersion,CURRENT_VERSION);");
  h += F("if(cmp===null){versionAction.textContent='UNKNOWN';versionAction.className='vervalue bad';btn.disabled=true;btn.textContent='VERSION UNKNOWN';return;}");
  h += F("if(cmp<0){");
  h += F("versionRelation='downgrade';versionAction.textContent='DOWNGRADE';versionAction.className='downgrade';");
  h += F("btn.textContent='DOWNGRADE TO '+detectedVersion;btn.disabled=false;");
  h += F("st.textContent='WARNING: selected '+detectedVersion+' is older than current '+CURRENT_VERSION+'.';st.className='bad';");
  h += F("}else if(cmp>0){");
  h += F("versionRelation='upgrade';versionAction.textContent='UPGRADE';versionAction.className='upgrade';");
  h += F("btn.textContent='UPLOAD '+detectedVersion;btn.disabled=false;");
  h += F("st.textContent='READY TO UPGRADE: '+CURRENT_VERSION+' -> '+detectedVersion;st.className='ok';");
  h += F("}else{");
  h += F("versionRelation='same';versionAction.textContent='SAME VERSION';versionAction.className='same';");
  h += F("btn.textContent='REINSTALL '+detectedVersion;btn.disabled=false;");
  h += F("st.textContent='SAME VERSION selected: '+detectedVersion+'.';st.className='muted';");
  h += F("}");
  h += F("});");

  h += F("async function waitForDinMeter(){");
  h += F("btn.disabled=true;btn.textContent='WAITING FOR DINMETER...';");
  h += F("for(let i=1;i<=40;i++){");
  h += F("try{");
  h += F("const r=await fetch('/health?t='+Date.now(),{cache:'no-store'});");
  h += F("if(r.ok){");
  h += F("const j=await r.json();");
  h += F("st.textContent='DINMETER BACK ONLINE - '+(j.version||'ONLINE');st.className='ok';");
  h += F("btn.disabled=false;btn.textContent='OPEN UPDATED DINMETER';");
  h += F("btn.onclick=()=>{location.href='/?t='+Date.now();};");
  h += F("return;");
  h += F("}");
  h += F("}catch(e){}");
  h += F("st.textContent='REBOOTING / RECONNECTING... '+i;st.className='ok';");
  h += F("await new Promise(r=>setTimeout(r,1000));");
  h += F("}");
  h += F("st.textContent='DINMETER NOT BACK YET - CHECK THE UNIT SCREEN';st.className='bad';");
  h += F("btn.disabled=false;btn.textContent='TRY OPEN DINMETER';");
  h += F("btn.onclick=()=>{location.href='/?t='+Date.now();};");
  h += F("}");

  h += F("async function rebootApply(){");
  h += F("btn.disabled=true;btn.textContent='REBOOTING...';");
  h += F("st.textContent='APPLYING VERIFIED FIRMWARE - DEVICE WILL REBOOT';st.className='ok';");
  h += F("try{await fetch('/reboot',{method:'POST',cache:'no-store'});}catch(e){}");
  h += F("await new Promise(r=>setTimeout(r,900));");
  h += F("waitForDinMeter();");
  h += F("}");

  h += F("f.addEventListener('submit',async(e)=>{");
  h += F("e.preventDefault();");
  h += F("if(rebootMode){rebootApply();return;}");
  h += F("if(!file.files.length){st.textContent='SELECT A .BIN FILE FIRST';st.className='bad';return;}");
  h += F("if(!detectedVersion){st.textContent='VERSION CHECK REQUIRED BEFORE UPLOAD';st.className='bad';return;}");
  h += F("if(versionRelation==='downgrade'){");
  h += F("if(!confirm('DOWNGRADE WARNING\\n\\nCurrent: '+CURRENT_VERSION+'\\nSelected: '+detectedVersion+'\\n\\nContinue?'))return;");
  h += F("}");
  h += F("const fw=file.files[0];");
  h += F("btn.disabled=true;file.disabled=true;");
  h += F("btn.textContent='PREPARING...';");
  h += F("st.textContent='CHECKING OTA PARTITION / FILE SIZE...';st.className='ok';");
  h += F("wrap.style.display='block';bar.style.width='0%';");

  h += F("let prep;");
  h += F("try{");
  h += F("prep=await fetch('/ota-begin?size='+encodeURIComponent(fw.size)+'&t='+Date.now(),{cache:'no-store'});");
  h += F("if(!prep.ok){throw new Error(await prep.text());}");
  h += F("}catch(err){");
  h += F("st.textContent='PREPARE FAILED: '+err.message;st.className='bad';");
  h += F("btn.disabled=false;file.disabled=false;btn.textContent='UPLOAD FIRMWARE';return;");
  h += F("}");

  h += F("btn.textContent='UPLOADING...';");
  h += F("st.textContent='UPLOAD STARTED - DO NOT POWER OFF';");
  h += F("const xhr=new XMLHttpRequest();");
  h += F("xhr.open('POST','/update',true);");

  h += F("xhr.upload.onprogress=(ev)=>{");
  h += F("if(ev.lengthComputable){");
  h += F("const p=Math.min(99,Math.round((ev.loaded/ev.total)*100));");
  h += F("bar.style.width=p+'%';");
  h += F("st.textContent='SENDING TO DINMETER... '+p+'%';");
  h += F("}");
  h += F("};");

  h += F("xhr.onload=()=>{");
  h += F("let j=null;try{j=JSON.parse(xhr.responseText);}catch(e){}");
  h += F("if(xhr.status>=200&&xhr.status<300&&j&&j.ok){");
  h += F("bar.style.width='100%';");
  h += F("st.textContent='VERIFIED ON DINMETER: '+j.bytes.toLocaleString()+' / '+j.expected.toLocaleString()+' bytes';");
  h += F("st.className='ok';");
  h += F("rebootMode=true;");
  h += F("file.disabled=true;btn.disabled=false;btn.textContent='REBOOT & APPLY';");
  h += F("}else{");
  h += F("const msg=(j&&j.error)?j.error:('HTTP '+xhr.status+' '+xhr.responseText);");
  h += F("st.textContent='UPDATE FAILED: '+msg;st.className='bad';");
  h += F("btn.disabled=false;file.disabled=false;btn.textContent='UPLOAD FIRMWARE';");
  h += F("}");
  h += F("};");

  h += F("xhr.onerror=()=>{");
  h += F("st.textContent='NETWORK CONNECTION LOST BEFORE DINMETER VERIFIED THE UPDATE';st.className='bad';");
  h += F("btn.disabled=false;file.disabled=false;btn.textContent='UPLOAD FIRMWARE';");
  h += F("};");

  h += F("const fd=new FormData();fd.append('firmware',fw,fw.name);xhr.send(fd);");
  h += F("});");
  h += F("</script>");

  h += F("<hr><h2>Wi-Fi Setup</h2>");
  h += F("<form method='POST' action='/savewifi'>");
  h += F("<label>SSID</label><input name='ssid' type='text' value='");
  h += htmlEscape(savedSsid);
  h += F("' required>");
  h += F("<label>Password</label><input name='pass' type='password' value=''>");
  h += F("<button type='submit'>SAVE WIFI</button></form>");
  h += F("<p class='muted'>Credentials are stored in ESP32 NVS, not in the sketch source.</p>");

  h += F("<hr><p class='muted'>Arduino IDE OTA hostname: <b>DinMeter-Synth</b><br>");
  h += F("Browser address on home Wi-Fi: <b>http://dinmeter.local/</b><br>");
  h += F("Emergency wired recovery: SYSTEM MENU -> <b>USB FLASH</b></p>");

  h += pageFooter();
  return h;
}

void startMdnsAndOta() {
  // ArduinoOTA owns the mDNS lifecycle.  The hostname also makes
  // http://dinmeter.local/ resolvable on the home LAN.
  ArduinoOTA.setHostname("DinMeter-Synth");

  ArduinoOTA.onStart([]() {
    updating = true;
    updateProgress = 0;
    statusText = "ARDUINO OTA UPDATING";
  });

  ArduinoOTA.onEnd([]() {
    updateProgress = 100;
    statusText = "ARDUINO OTA COMPLETE";
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    if (total > 0) {
      updateProgress = (int)((progress * 100U) / total);
    }
  });

  ArduinoOTA.onError([](ota_error_t error) {
    updating = false;
    statusText = "ARDUINO OTA ERROR " + String((int)error);
  });

  ArduinoOTA.begin();
}

void registerWebRoutes() {
  if (routesRegistered) {
    server.begin();
    return;
  }
  routesRegistered = true;

  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html; charset=utf-8", rootPage());
  });

  server.on("/health", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json; charset=utf-8",
                "{\"ok\":true,\"version\":\"v1.8.3\"}");
  });

  server.on("/ota-status", HTTP_GET, []() {
    String j = "{\"updating\":";
    j += updating ? "true" : "false";
    j += ",\"ready\":";
    j += webUpdateReadyToReboot ? "true" : "false";
    j += ",\"expected\":";
    j += String((unsigned long)webExpectedSize);
    j += ",\"written\":";
    j += String((unsigned long)webBytesWritten);
    j += ",\"errorCode\":";
    j += String((unsigned int)webUpdateError);
    j += ",\"error\":\"";
    j += htmlEscape(webUpdateErrorText);
    j += "\"}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json; charset=utf-8", j);
  });

  // Phase 1: browser declares the exact firmware file size.
  // This allows Update.end(false) to reject partial/truncated transfers.
  server.on("/ota-begin", HTTP_GET, []() {
    if (updating || webUpdateReadyToReboot) {
      server.send(409, "text/plain; charset=utf-8",
                  "An OTA operation is already active or waiting for reboot.");
      return;
    }

    if (!server.hasArg("size")) {
      server.send(400, "text/plain; charset=utf-8", "Missing firmware size.");
      return;
    }

    size_t requested = (size_t)server.arg("size").toInt();
    size_t available = ESP.getFreeSketchSpace();

    if (requested == 0) {
      server.send(400, "text/plain; charset=utf-8", "Firmware size is zero.");
      return;
    }

    if (available == 0 || requested > available) {
      String msg = "Firmware too large. Requested=";
      msg += String((unsigned long)requested);
      msg += " bytes, OTA partition=";
      msg += String((unsigned long)available);
      msg += " bytes.";
      server.send(413, "text/plain; charset=utf-8", msg);
      return;
    }

    webExpectedSize = requested;
    webBytesWritten = 0;
    webUpdateOk = false;
    webUpdateReadyToReboot = false;
    webUpdateError = 0;
    webUpdateErrorText = "";
    updateProgress = 0;
    statusText = "WEB OTA PREPARED";

    String j = "{\"ok\":true,\"size\":";
    j += String((unsigned long)requested);
    j += ",\"partition\":";
    j += String((unsigned long)available);
    j += "}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json; charset=utf-8", j);
  });

  // Phase 2: multipart firmware upload.
  server.on(
    "/update",
    HTTP_POST,
    []() {
      // Final HTTP response is sent only after the upload callback has
      // either fully verified the firmware or recorded a precise failure.
      if (webUpdateOk && webUpdateReadyToReboot) {
        String j = "{\"ok\":true,\"bytes\":";
        j += String((unsigned long)webBytesWritten);
        j += ",\"expected\":";
        j += String((unsigned long)webExpectedSize);
        j += ",\"message\":\"verified; reboot required\"}";
        server.sendHeader("Cache-Control", "no-store");
        server.send(200, "application/json; charset=utf-8", j);
      } else {
        String j = "{\"ok\":false,\"bytes\":";
        j += String((unsigned long)webBytesWritten);
        j += ",\"expected\":";
        j += String((unsigned long)webExpectedSize);
        j += ",\"errorCode\":";
        j += String((unsigned int)webUpdateError);
        j += ",\"error\":\"";
        j += htmlEscape(webUpdateErrorText.length() ? webUpdateErrorText : String("Firmware was not verified."));
        j += "\"}";
        server.sendHeader("Cache-Control", "no-store");
        server.send(500, "application/json; charset=utf-8", j);

        updating = false;
        webUpdateReadyToReboot = false;
      }
    },
    []() {
      HTTPUpload& upload = server.upload();

      if (upload.status == UPLOAD_FILE_START) {
        updating = true;
        webUpdateOk = false;
        webUpdateReadyToReboot = false;
        webBytesWritten = 0;
        webUpdateError = 0;
        webUpdateErrorText = "";
        updateProgress = 0;
        statusText = "WEB OTA BEGIN";

        if (webExpectedSize == 0) {
          webUpdateError = UPDATE_ERROR_SIZE;
          webUpdateErrorText = "OTA was not prepared with an exact file size.";
          statusText = "WEB OTA SIZE ERROR";
          return;
        }

        // Exact-size begin. This selects the inactive OTA app partition.
        if (!Update.begin(webExpectedSize, U_FLASH)) {
          webUpdateError = Update.getError();
          webUpdateErrorText = Update.errorString();
          statusText = "WEB OTA BEGIN ERROR " + String((int)webUpdateError);
          return;
        }
      }
      else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.hasError()) {
          webUpdateError = Update.getError();
          webUpdateErrorText = Update.errorString();
          return;
        }

        size_t written = Update.write(upload.buf, upload.currentSize);
        webBytesWritten += written;

        if (written != upload.currentSize) {
          webUpdateError = Update.getError();
          webUpdateErrorText = Update.errorString();
          if (webUpdateErrorText.length() == 0) {
            webUpdateErrorText = "Flash write returned fewer bytes than received.";
          }
          statusText = "WEB OTA WRITE ERROR";
          return;
        }

        if (webExpectedSize > 0) {
          updateProgress = (int)((webBytesWritten * 100ULL) / webExpectedSize);
          if (updateProgress > 99) updateProgress = 99;
        }
        statusText = "WEB OTA WRITING";
      }
      else if (upload.status == UPLOAD_FILE_END) {
        if (Update.hasError()) {
          webUpdateError = Update.getError();
          webUpdateErrorText = Update.errorString();
          webUpdateOk = false;
          updating = false;
          statusText = "WEB OTA ERROR " + String((int)webUpdateError);
          return;
        }

        if (webBytesWritten != webExpectedSize) {
          Update.abort();
          webUpdateError = UPDATE_ERROR_SIZE;
          webUpdateErrorText = "Received firmware byte count does not match the selected file.";
          webUpdateOk = false;
          updating = false;
          statusText = "WEB OTA BYTE COUNT ERROR";
          return;
        }

        // IMPORTANT: false means "do NOT accept an incomplete image".
        webUpdateOk = Update.end(false);

        if (!webUpdateOk) {
          webUpdateError = Update.getError();
          webUpdateErrorText = Update.errorString();
          updating = false;
          statusText = "WEB OTA END ERROR " + String((int)webUpdateError);
          return;
        }

        updateProgress = 100;
        statusText = "WEB OTA VERIFIED - REBOOT";
        webUpdateReadyToReboot = true;

        // Keep 'updating' true so the physical encoder cannot accidentally
        // exit MAINTENANCE while a verified image is waiting to be applied.
        updating = true;
      }
      else if (upload.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        updating = false;
        webUpdateOk = false;
        webUpdateReadyToReboot = false;
        webUpdateError = UPDATE_ERROR_ABORT;
        webUpdateErrorText = "Browser aborted the firmware upload.";
        statusText = "WEB OTA ABORTED";
      }
    }
  );

  // Phase 3: only after V2 verification succeeds do we permit reboot/apply.
  server.on("/reboot", HTTP_POST, []() {
    if (!webUpdateReadyToReboot || !webUpdateOk) {
      server.send(409, "application/json; charset=utf-8",
                  "{\"ok\":false,\"error\":\"No verified firmware is waiting for reboot.\"}");
      return;
    }

    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json; charset=utf-8",
                "{\"ok\":true,\"message\":\"reboot scheduled\"}");

    statusText = "WEB OTA APPLYING";
    restartAt = millis() + 1200;
  });

  server.on("/savewifi", HTTP_POST, []() {
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");
    ssid.trim();

    if (ssid.length() == 0 || ssid.length() > 64 || pass.length() > 64) {
      server.send(400, "text/plain; charset=utf-8", "Invalid SSID/password.");
      return;
    }

    wifiPrefs.putString("ssid", ssid);
    wifiPrefs.putString("pass", pass);
    savedSsid = ssid;
    savedPass = pass;

    String h = pageHeader("Wi-Fi Saved");
    h += F("<div class='warn'>WIFI SAVED</div><br>");
    h += F("<p class='ok'>SSID/password saved to NVS.</p>");
    h += F("<p>The Din Meter will reboot into normal mode in about 2 seconds.</p>");
    h += pageFooter();

    server.send(200, "text/html; charset=utf-8", h);
    restartAt = millis() + 2000;
  });

  server.onNotFound([]() {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
  });

  server.begin();
}

void startAccessPoint(WifiMaintMode mode) {
  WiFi.mode(WIFI_OFF);
  delay(50);

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);

  WiFi.softAP(AP_SSID);
  delay(150);

  runtimeMode = mode;
  statusText = (mode == WifiMaintMode::SETUP_AP)
                 ? "WIFI SETUP READY"
                 : "HOME WIFI FAILED - AP FALLBACK";

  registerWebRoutes();
  startMdnsAndOta();
}

}  // namespace

void wifiMaintInit() {
  wifiPrefs.begin(PREF_NS, false);
  savedSsid = wifiPrefs.getString("ssid", "");
  savedPass = wifiPrefs.getString("pass", "");

  WiFi.mode(WIFI_OFF);
  runtimeMode = WifiMaintMode::OFF;
  statusText = "OFF";
}

void wifiMaintStartMaintenance() {
  wifiMaintStop();

  savedSsid = wifiPrefs.getString("ssid", "");
  savedPass = wifiPrefs.getString("pass", "");

  if (savedSsid.length() == 0) {
    startAccessPoint(WifiMaintMode::MAINT_AP);
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname("DinMeter-Synth");

  statusText = "CONNECTING " + savedSsid;
  WiFi.begin(savedSsid.c_str(), savedPass.c_str());

  uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - started < STA_TIMEOUT_MS) {
    delay(50);
  }

  if (WiFi.status() == WL_CONNECTED) {
    runtimeMode = WifiMaintMode::MAINT_STA;
    statusText = "OTA READY";
    registerWebRoutes();
    startMdnsAndOta();
    return;
  }

  WiFi.disconnect(true);
  delay(100);
  startAccessPoint(WifiMaintMode::MAINT_AP);
}

void wifiMaintStartSetup() {
  wifiMaintStop();
  startAccessPoint(WifiMaintMode::SETUP_AP);
}

void wifiMaintStop() {
  if (runtimeMode == WifiMaintMode::OFF) return;

  server.stop();
  ArduinoOTA.end();

  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  runtimeMode = WifiMaintMode::OFF;
  updating = false;
  updateProgress = 0;
  webUpdateOk = false;
  webUpdateReadyToReboot = false;
  webExpectedSize = 0;
  webBytesWritten = 0;
  webUpdateError = 0;
  webUpdateErrorText = "";
  statusText = "OFF";
  restartAt = 0;

  delay(50);
}

void wifiMaintLoop() {
  if (runtimeMode == WifiMaintMode::OFF) return;

  server.handleClient();

  // Do not let ArduinoOTA compete with the Web OTA state machine while
  // a browser update is writing or waiting for reboot.
  if (!updating && !webUpdateReadyToReboot) {
    ArduinoOTA.handle();
  }

  if (restartAt != 0 && (int32_t)(millis() - restartAt) >= 0) {
    delay(100);
    ESP.restart();
  }
}

bool wifiMaintActive() {
  return runtimeMode != WifiMaintMode::OFF;
}

bool wifiMaintUpdating() {
  return updating;
}

bool wifiMaintHasCredentials() {
  return savedSsid.length() > 0;
}

bool wifiMaintStaConnected() {
  return runtimeMode == WifiMaintMode::MAINT_STA &&
         WiFi.status() == WL_CONNECTED;
}

WifiMaintMode wifiMaintMode() {
  return runtimeMode;
}

String wifiMaintIp() {
  if (runtimeMode == WifiMaintMode::MAINT_STA) {
    return WiFi.localIP().toString();
  }
  if (runtimeMode == WifiMaintMode::MAINT_AP ||
      runtimeMode == WifiMaintMode::SETUP_AP) {
    return WiFi.softAPIP().toString();
  }
  return "-";
}

String wifiMaintModeText() {
  switch (runtimeMode) {
    case WifiMaintMode::MAINT_STA: return "HOME WIFI";
    case WifiMaintMode::MAINT_AP:  return "AP FALLBACK";
    case WifiMaintMode::SETUP_AP:  return "WIFI SETUP AP";
    default:                       return "OFF";
  }
}

String wifiMaintSavedSsid() {
  return savedSsid;
}

int wifiMaintProgress() {
  return updateProgress;
}

String wifiMaintStatus() {
  return statusText;
}

/*
  ======================================================================
  Module : DinMeter Wi-Fi / OTA Maintenance
  Version: v1.8.3
  END
  ======================================================================
*/
