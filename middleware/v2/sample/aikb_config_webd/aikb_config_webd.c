#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define AIKB_PROFILE_CONFIG_DIR "/mnt/system/etc/aikb"
#define AIKB_PROFILE_CONFIG "/mnt/system/etc/aikb/profiles.conf"
#define AIKB_PROFILE_BOOT_CONFIG "/boot/aikb_profiles.conf"
#define AIKB_PROFILE_ID_MAX 16
#define AIKB_PROFILE_SOUND_VOLUME_DEFAULT 35u
#define KEY_COUNT 7
#define VIBE_ACTION_NONE 0xffu

#define HID_MOD_LEFT_CTRL 0x01u
#define HID_USAGE_A 0x04u
#define HID_USAGE_C 0x06u
#define HID_USAGE_V 0x19u
#define HID_USAGE_ENTER 0x28u
#define HID_USAGE_ESC 0x29u
#define HID_USAGE_TAB 0x2bu
#define HID_USAGE_SPACE 0x2cu

#define DEFAULT_PORT 80
#define MAX_LISTENERS 8
#define REQ_BUF_SIZE 8192
#define BODY_LIMIT 4096

struct key_binding {
	uint8_t modifier;
	uint8_t usage;
};

struct profile_config {
	char active_profile[AIKB_PROFILE_ID_MAX];
	bool sound_enabled;
	unsigned sound_volume;
	struct key_binding keymap[KEY_COUNT];
	uint8_t actionmap[KEY_COUNT];
};

struct listener {
	int fd;
	char ifname[IFNAMSIZ];
	char ip[INET_ADDRSTRLEN];
};

static volatile sig_atomic_t g_stop;

struct key_preset {
	const char *id;
	uint8_t modifier;
	uint8_t usage;
};

struct vibe_action_preset {
	const char *id;
	uint8_t bit;
};

static const struct key_preset g_key_presets[] = {
	{ "none", 0, 0 },
	{ "esc", 0, HID_USAGE_ESC },
	{ "tab", 0, HID_USAGE_TAB },
	{ "space", 0, HID_USAGE_SPACE },
	{ "enter", 0, HID_USAGE_ENTER },
	{ "ctrl_a", HID_MOD_LEFT_CTRL, HID_USAGE_A },
	{ "ctrl_c", HID_MOD_LEFT_CTRL, HID_USAGE_C },
	{ "ctrl_v", HID_MOD_LEFT_CTRL, HID_USAGE_V },
};

static const struct vibe_action_preset g_vibe_action_presets[] = {
	{ "none", VIBE_ACTION_NONE },
	{ "reject", 0 },
	{ "voice", 1 },
	{ "session", 2 },
	{ "review", 3 },
	{ "vote_review", 3 },
	{ "sleep", 4 },
	{ "agent_model", 4 },
	{ "multi", 5 },
	{ "multi_function", 5 },
	{ "confirm", 6 },
};

static const struct key_binding g_default_keymap[KEY_COUNT] = {
	{ 0, HID_USAGE_ESC },
	{ 0, HID_USAGE_TAB },
	{ 0, HID_USAGE_SPACE },
	{ HID_MOD_LEFT_CTRL, HID_USAGE_A },
	{ HID_MOD_LEFT_CTRL, HID_USAGE_C },
	{ HID_MOD_LEFT_CTRL, HID_USAGE_V },
	{ 0, HID_USAGE_ENTER },
};

static const uint8_t g_default_actionmap[KEY_COUNT] = {
	0, 1, 2, 3, 4, 5, 6,
};

static const char k_index_html[] =
"<!doctype html>\n"
"<html lang=\"zh-CN\">\n"
"<head>\n"
"<meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
"<title>AIKB Control</title>\n"
"<style>\n"
":root{color-scheme:light;--bg:#e4e5e3;--panel:#ffffff;--ink:#20252b;--muted:#66717c;--line:#d8dee6;--soft:#f7f9fb;--metal:#2b2d2f;--metal2:#17191c;--key:#d1d0c9;--key2:#aaa89f;--accent:#b88619;--accent2:#1d8a6a;--ok:#0f7a4f;--warn:#8a5a00;--shadow:0 16px 38px rgba(36,48,61,.10);}\n"
"*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:14px/1.5 ui-sans-serif,-apple-system,BlinkMacSystemFont,\"Segoe UI\",sans-serif}main{max-width:1040px;margin:0 auto;padding:24px 16px 34px}header{display:flex;align-items:flex-start;justify-content:space-between;gap:20px;margin-bottom:16px}.brand{display:flex;gap:12px;align-items:flex-start}.mark{width:38px;height:38px;border-radius:8px;background:#20252b;color:#fff;display:grid;place-items:center;font-weight:800}.eyebrow{font-size:12px;color:var(--muted);text-transform:uppercase;letter-spacing:0}h1{margin:1px 0 0;font-size:25px;font-weight:750;letter-spacing:0}.sub{margin-top:5px;color:var(--muted)}.topright{display:grid;gap:9px;justify-items:end;max-width:440px}.statusbox{text-align:right;color:var(--muted);font-size:12px}.status{min-height:18px;color:var(--muted)}.actions{display:flex;gap:8px;flex-wrap:wrap;justify-content:flex-end}.panel{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:17px;margin:12px 0;box-shadow:var(--shadow)}.panel.compact{box-shadow:none;background:rgba(255,255,255,.72)}.grid{display:grid;grid-template-columns:1fr 1.12fr;gap:14px}.field{background:var(--soft);border:1px solid #e3e8ef;border-radius:8px;padding:13px}.field.wide{grid-column:1/-1}.layout-field{padding:14px}label{display:block;color:var(--muted);font-size:12px;margin-bottom:7px}select{width:100%;height:38px;border:1px solid var(--line);border-radius:6px;background:#fff;color:var(--ink);padding:0 10px;font:inherit}input[type=range]{width:100%;accent-color:var(--accent)}.audio-field{display:grid;gap:10px}.audioTop,.volumeRow{display:flex;align-items:center;justify-content:space-between;gap:14px}.audioTop label{margin:0}.volumeRow input{min-width:0}.toggle{display:flex;align-items:center;justify-content:flex-end;gap:12px;min-height:30px}.switch{appearance:none;width:46px;height:26px;border-radius:999px;background:#b8c2cd;position:relative;transition:.18s;flex:0 0 auto}.switch:checked{background:var(--accent2)}.switch:before{content:\"\";position:absolute;width:20px;height:20px;left:3px;top:3px;border-radius:50%;background:#fff;box-shadow:0 1px 4px rgba(0,0,0,.22);transition:.18s}.switch:checked:before{transform:translateX(20px)}.value{min-width:54px;text-align:right;font-size:22px;font-weight:720}.ok{color:var(--ok)}.dirty{color:var(--warn)}button{min-height:36px;padding:0 13px;border:1px solid var(--line);border-radius:6px;background:#fff;color:var(--ink);font:inherit;cursor:pointer}button.primary{border-color:#1f5f8f;background:#1f5f8f;color:#fff}button:disabled{opacity:.45;cursor:default}.urlbar{display:flex;gap:6px;flex-wrap:wrap;justify-content:flex-end}.chip{border:1px solid #d8e2ec;background:#f8fbfd;border-radius:999px;padding:3px 8px;color:#44515e;font-size:12px}.maphead{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:10px}.modebadge{border:1px solid #d2b55f;background:#fff8df;color:#6f4b00;border-radius:999px;padding:3px 9px;font-size:12px;font-weight:720}.deck{display:grid;grid-template-columns:minmax(0,1fr) 168px;grid-template-areas:\"screen enc\" \"keys enc\";gap:14px;background:linear-gradient(145deg,var(--metal),var(--metal2));border:1px solid #0f1113;border-radius:8px;padding:16px;box-shadow:inset 0 1px 0 rgba(255,255,255,.12),0 18px 34px rgba(18,22,27,.22)}.screen{grid-area:screen;min-height:126px;border-radius:8px;border:1px solid #3f4347;background:#101214;color:#d9c06d;padding:14px;box-shadow:inset 0 0 0 4px #191c1f}.screenbar{display:flex;align-items:center;justify-content:space-between;color:#998f64;font-size:11px}.screenline{height:1px;background:#6e6751;margin:11px 0}.screentitle{font-size:24px;font-weight:800;letter-spacing:0}.screensub{color:#e8e1bf;font-size:12px;margin-top:4px}.encoder{grid-area:enc;display:flex;flex-direction:column;align-items:center;justify-content:flex-start;gap:10px;min-width:0}.knob{width:132px;max-width:100%;aspect-ratio:1;border-radius:50%;background:repeating-conic-gradient(from 2deg,#44484c 0 5deg,#202326 5deg 9deg),radial-gradient(circle at 38% 30%,#797d80 0,#5d6165 48%,#282b2f 76%);box-shadow:0 12px 20px rgba(0,0,0,.35),inset 0 2px 2px rgba(255,255,255,.18)}.encoderlabel{text-align:center;color:#d7d9d8;font-weight:800}.encodertext{color:#aeb4b5;font-size:12px;text-align:center}.keygrid{grid-area:keys;display:grid;grid-template-columns:repeat(3,minmax(0,1fr));grid-template-areas:\"k0 k1 k6\" \"k2 k3 k6\" \"k4 k5 k6\";gap:10px;align-items:stretch}.keycell{min-width:0;min-height:78px;border-radius:8px;background:linear-gradient(180deg,var(--key),var(--key2));border:1px solid #87877f;box-shadow:0 7px 0 #787870,0 14px 18px rgba(0,0,0,.24),inset 0 2px 0 rgba(255,255,255,.32);padding:8px;display:flex;flex-direction:column;justify-content:space-between}.keycell b{display:block;color:#393a38;font-size:15px;line-height:1.1;min-height:17px}.keycell span{display:block;color:#65655f;font-size:11px;font-weight:720;margin-bottom:4px}.keycell select{height:34px;background:rgba(255,255,255,.78)}.key-0{grid-area:k0}.key-1{grid-area:k1}.key-2{grid-area:k2}.key-3{grid-area:k3}.key-4{grid-area:k4}.key-5{grid-area:k5}.key-6{grid-area:k6;min-height:auto}.loglabel{display:flex;align-items:center;justify-content:space-between;color:var(--muted);margin-bottom:7px}pre{margin:0;padding:11px;border-radius:8px;background:#18202a;color:#d9e2ec;min-height:96px;max-height:180px;overflow:auto;white-space:pre-wrap;font-size:12px}@media(max-width:760px){header{display:block}.topright{justify-items:start;max-width:none;margin-top:12px}.statusbox{text-align:left}.urlbar{justify-content:flex-start}.actions{justify-content:flex-start}.grid{display:block}.field{margin-bottom:12px}.deck{display:block}.screen{margin-bottom:14px}.encoder{align-items:flex-start;margin-bottom:14px}.knob{width:104px}.keygrid{grid-template-columns:repeat(2,minmax(0,1fr));grid-template-areas:\"k0 k1\" \"k2 k3\" \"k4 k5\" \"k6 k6\"}.keycell{min-height:88px}}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<main>\n"
"<header><div class=\"brand\"><div class=\"mark\">A</div><div><div class=\"eyebrow\">AIKB</div><h1>Device Control</h1><div class=\"sub\">Profile · Sound · Runtime</div></div></div><div class=\"topright\"><div class=\"statusbox\"><div id=\"status\" class=\"status\">正在读取配置...</div><div id=\"urls\" class=\"urlbar\"></div></div><div class=\"actions\"><button id=\"refresh\" class=\"primary\">刷新</button><button id=\"reset\" disabled>恢复默认</button></div></div></header>\n"
"<section class=\"panel\"><div class=\"grid\"><div class=\"field\"><label for=\"profile\">当前模式</label><select id=\"profile\" disabled><option value=\"normal\">普通模式</option><option value=\"vibe\">vibe-coding</option><option value=\"custom1\">自定义 1</option><option value=\"custom2\">自定义 2</option><option value=\"custom3\">自定义 3</option></select></div><div class=\"field audio-field\"><div class=\"audioTop\"><label>音效 / 音量</label><div class=\"toggle\"><span id=\"soundText\">读取中</span><input id=\"sound\" class=\"switch\" type=\"checkbox\" disabled></div></div><div class=\"volumeRow\"><input id=\"volume\" type=\"range\" min=\"0\" max=\"100\" step=\"1\" value=\"35\" disabled><span class=\"value\" id=\"volumeText\">--</span></div></div><div class=\"field wide layout-field\"><div class=\"maphead\"><label id=\"mapLabel\">7 键映射</label><span id=\"modeBadge\" class=\"modebadge\">--</span></div><div class=\"deck\"><div class=\"screen\"><div class=\"screenbar\"><span id=\"screenMode\">--</span><span id=\"screenSound\">--</span></div><div class=\"screenline\"></div><div id=\"screenTitle\" class=\"screentitle\">AIKB</div><div id=\"screenSub\" class=\"screensub\">KEY MAP</div></div><div class=\"encoder\"><div class=\"knob\"></div><div class=\"encoderlabel\">ENCODER</div><div id=\"encoderText\" class=\"encodertext\">--</div></div><div id=\"keyGrid\" class=\"keygrid\"></div></div></div></div></section>\n"
"<section class=\"panel compact\"><div class=\"loglabel\"><span>设备响应</span><button id=\"clearLog\">清空</button></div><pre id=\"log\"></pre></section>\n"
"</main>\n"
"<script>\n"
"const defaultKeys=['esc','tab','space','ctrl_a','ctrl_c','ctrl_v','enter'];\n"
"const defaultActions=['reject','voice','session','review','sleep','multi','confirm'];\n"
"const keyOptions=[['none','不发送'],['esc','Esc'],['tab','Tab'],['space','Space'],['enter','Enter'],['ctrl_a','Ctrl+A'],['ctrl_c','Ctrl+C'],['ctrl_v','Ctrl+V']];\n"
"const actionOptions=[['none','不发送'],['reject','REJECT'],['voice','VOICE'],['session','SESSION'],['review','REVIEW'],['sleep','SLEEP'],['multi','MULTI'],['confirm','CONFIRM']];\n"
"let state={active_profile:'vibe',sound_enabled:0,sound_volume:35,keys:defaultKeys.slice(),actions:defaultActions.slice()};\n"
"let saveTimer=null;let loading=false;let saving=false;let pendingSave=false;let mapMode=null;let controlsEnabled=false;\n"
"const $=id=>document.getElementById(id);const log=t=>{$('log').textContent=new Date().toLocaleTimeString()+' '+t+'\\n'+$('log').textContent};\n"
"function setStatus(t,c){$('status').className='status '+(c||'');$('status').textContent=t;}\n"
"function optionText(opts,v){const hit=opts.find(o=>o[0]===v);return hit?hit[1]:String(v||'--').toUpperCase();}\n"
"function renderMapControls(mode){if(mapMode===mode&&$('keyGrid').children.length)return; mapMode=mode; const opts=mode==='action'?actionOptions:keyOptions; $('mapLabel').textContent=mode==='action'?'Vibe 动作映射':'键盘输出映射'; $('keyGrid').innerHTML=defaultKeys.map((_,i)=>'<div class=\"keycell key-'+i+'\"><div><span>KEY '+i+'</span><b data-key-title=\"'+i+'\">--</b></div><select data-map=\"'+mode+'\" '+(controlsEnabled?'':'disabled')+'>'+opts.map(o=>'<option value=\"'+o[0]+'\">'+o[1]+'</option>').join('')+'</select></div>').join(''); document.querySelectorAll('[data-map]').forEach(el=>el.onchange=scheduleSave);}\n"
"function setControls(on){controlsEnabled=on;['profile','sound','volume','reset'].forEach(id=>$(id).disabled=!on);document.querySelectorAll('[data-map]').forEach(el=>el.disabled=!on);}\n"
"function canSync(){return !saving&&!pendingSave&&!saveTimer;}\n"
"function collect(){const next={active_profile:$('profile').value,sound_enabled:$('sound').checked?1:0,sound_volume:Number($('volume').value),keys:(state.keys||defaultKeys).slice(),actions:(state.actions||defaultActions).slice()}; const vals=Array.from(document.querySelectorAll('[data-map]')).map(el=>el.value); if(mapMode==='action')next.actions=vals; else next.keys=vals; return next;}\n"
"function renderHardware(mode,vals){const opts=mode==='action'?actionOptions:keyOptions; document.querySelectorAll('[data-key-title]').forEach((el,i)=>{el.textContent=optionText(opts,vals[i]||(mode==='action'?defaultActions[i]:defaultKeys[i]))}); const vibe=mode==='action'; $('modeBadge').textContent=vibe?'VIBE':'KEYBOARD'; $('screenMode').textContent=vibe?'VIBE CODING':'KEYBOARD'; $('screenTitle').textContent=vibe?'ACTIONS':'KEYBOARD'; $('screenSub').textContent=vibe?'SESSION / VOICE / SLEEP':'USB HID OUTPUT'; $('encoderText').textContent=vibe?'ROTATE / PRESS':'IDLE'; $('screenSound').textContent=Number(state.sound_enabled)!==0?'SOUND ON':'SOUND OFF';}\n"
"function render(){ loading=true; const profile=state.active_profile||'vibe'; $('profile').value=profile; const mode=profile==='vibe'?'action':'key'; renderMapControls(mode); const vals=mode==='action'?(state.actions||defaultActions):(state.keys||defaultKeys); $('sound').checked=Number(state.sound_enabled)!==0; $('volume').value=String(state.sound_volume??35); $('volumeText').textContent=$('volume').value+'%'; $('soundText').textContent=$('sound').checked?'启用提示音':'关闭提示音'; document.querySelectorAll('[data-map]').forEach((el,i)=>{el.value=vals[i]||(mode==='action'?defaultActions[i]:defaultKeys[i])}); renderHardware(mode,vals); $('urls').innerHTML=(state.urls||[]).map(u=>'<span class=\"chip\">'+u+'</span>').join(''); loading=false; }\n"
"async function readConfig(reason){ if(!canSync()&&reason==='sync')return; if(reason==='init'){setControls(false);setStatus('正在识别设备状态...','dirty');} const r=await fetch('/api/profile',{cache:'no-store'}); if(!r.ok)throw new Error(await r.text()); state=await r.json(); render(); setControls(true); setStatus((reason==='sync'?'已同步 ':'已连接 ')+(reason==='sync'?new Date().toLocaleTimeString():location.origin),'ok'); if(reason!=='sync')log('< '+JSON.stringify(state)); }\n"
"async function saveNow(){ saveTimer=null; if(saving){pendingSave=true;return;} saving=true; pendingSave=false; const next=collect(); const body=new URLSearchParams(); body.set('active_profile',next.active_profile); body.set('sound_enabled',String(next.sound_enabled)); body.set('sound_volume',String(next.sound_volume)); next.keys.forEach((v,i)=>body.set('key'+i,v)); next.actions.forEach((v,i)=>body.set('action'+i,v)); setStatus('正在保存...','dirty'); const r=await fetch('/api/profile',{method:'POST',body}); const text=await r.text(); if(!r.ok)throw new Error(text); state=JSON.parse(text); render(); setStatus('已保存','ok'); log('> '+body.toString()); log('< '+text); saving=false; if(pendingSave){clearTimeout(saveTimer);saveTimer=setTimeout(()=>guard(saveNow()),250);} }\n"
"function scheduleSave(){ if(loading)return; const next=collect(); state={...state,...next}; render(); setStatus('等待保存...','dirty'); pendingSave=true; clearTimeout(saveTimer); saveTimer=setTimeout(()=>guard(saveNow()),700); }\n"
"async function resetConfig(){ clearTimeout(saveTimer); saveTimer=null; pendingSave=false; const r=await fetch('/api/reset',{method:'POST'}); const text=await r.text(); if(!r.ok)throw new Error(text); state=JSON.parse(text); render(); setStatus('已恢复默认','ok'); log('> reset'); log('< '+text); }\n"
"function guard(p){p.catch(e=>{ saving=false; setStatus(e.message,''); log('! '+e.message); });}\n"
"$('refresh').onclick=()=>guard(readConfig('manual')); $('reset').onclick=()=>guard(resetConfig()); $('clearLog').onclick=()=>{$('log').textContent=''}; $('profile').onchange=scheduleSave; $('sound').onchange=scheduleSave; $('volume').oninput=scheduleSave; document.addEventListener('visibilitychange',()=>{if(!document.hidden)guard(readConfig('focus'))}); window.addEventListener('focus',()=>guard(readConfig('focus'))); setInterval(()=>{if(!document.hidden)guard(readConfig('sync'))},3000); setControls(false); guard(readConfig('init'));\n"
"</script>\n"
"</body>\n"
"</html>\n";

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static char *trim_ascii(char *s)
{
	char *end;

	while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
		s++;
	end = s + strlen(s);
	while (end > s &&
	       (end[-1] == ' ' || end[-1] == '\t' ||
		end[-1] == '\r' || end[-1] == '\n')) {
		end--;
	}
	*end = '\0';
	return s;
}

static bool profile_id_valid(const char *id)
{
	return !strcmp(id, "normal") || !strcmp(id, "vibe") ||
	       !strcmp(id, "custom1") || !strcmp(id, "custom2") ||
	       !strcmp(id, "custom3");
}

static const char *key_binding_id(const struct key_binding *binding)
{
	for (size_t i = 0; i < sizeof(g_key_presets) / sizeof(g_key_presets[0]); i++) {
		if (g_key_presets[i].modifier == binding->modifier &&
		    g_key_presets[i].usage == binding->usage)
			return g_key_presets[i].id;
	}
	return NULL;
}

static void key_binding_format(const struct key_binding *binding,
			       char *buf, size_t len)
{
	const char *id = key_binding_id(binding);

	if (id) {
		snprintf(buf, len, "%s", id);
		return;
	}
	snprintf(buf, len, "mod:%02x,usage:%02x",
		 binding->modifier, binding->usage);
}

static const char *vibe_action_id(uint8_t bit)
{
	for (size_t i = 0; i < sizeof(g_vibe_action_presets) / sizeof(g_vibe_action_presets[0]); i++) {
		if (g_vibe_action_presets[i].bit == bit)
			return g_vibe_action_presets[i].id;
	}
	return NULL;
}

static void vibe_action_format(uint8_t bit, char *buf, size_t len)
{
	const char *id = vibe_action_id(bit);

	if (id) {
		snprintf(buf, len, "%s", id);
		return;
	}
	snprintf(buf, len, "bit:%02x", bit);
}

static bool parse_hex_byte(const char *value, unsigned *out)
{
	char *end = NULL;
	unsigned long v;

	errno = 0;
	v = strtoul(value, &end, 16);
	if (errno || !end || *end || v > 0xfful)
		return false;
	*out = (unsigned)v;
	return true;
}

static bool parse_key_binding_value(const char *value,
				    struct key_binding *binding)
{
	char tmp[64];
	char *mod_text;
	char *usage_text;
	char *sep;
	unsigned mod;
	unsigned usage;

	for (size_t i = 0; i < sizeof(g_key_presets) / sizeof(g_key_presets[0]); i++) {
		if (!strcmp(value, g_key_presets[i].id)) {
			binding->modifier = g_key_presets[i].modifier;
			binding->usage = g_key_presets[i].usage;
			return true;
		}
	}

	snprintf(tmp, sizeof(tmp), "%s", value);
	mod_text = tmp;
	if (!strncmp(mod_text, "mod:", 4))
		mod_text += 4;
	sep = strchr(mod_text, ',');
	if (!sep)
		sep = strchr(mod_text, ':');
	if (!sep)
		return false;
	*sep = '\0';
	usage_text = sep + 1;
	if (!strncmp(usage_text, "usage:", 6))
		usage_text += 6;
	if (!parse_hex_byte(mod_text, &mod) ||
	    !parse_hex_byte(usage_text, &usage))
		return false;
	binding->modifier = (uint8_t)mod;
	binding->usage = (uint8_t)usage;
	return true;
}

static bool parse_vibe_action_value(const char *value, uint8_t *bit)
{
	unsigned parsed;

	for (size_t i = 0; i < sizeof(g_vibe_action_presets) / sizeof(g_vibe_action_presets[0]); i++) {
		if (!strcmp(value, g_vibe_action_presets[i].id)) {
			*bit = g_vibe_action_presets[i].bit;
			return true;
		}
	}
	if (!strncmp(value, "bit:", 4))
		value += 4;
	if (!parse_hex_byte(value, &parsed))
		return false;
	if (parsed >= KEY_COUNT && parsed != VIBE_ACTION_NONE)
		return false;
	*bit = (uint8_t)parsed;
	return true;
}

static int profile_key_index(const char *key)
{
	char *end = NULL;
	unsigned long idx;

	if (strncmp(key, "key", 3) != 0)
		return -1;
	errno = 0;
	idx = strtoul(key + 3, &end, 10);
	if (errno || !end || *end || idx >= KEY_COUNT)
		return -1;
	return (int)idx;
}

static int profile_action_index(const char *key)
{
	char *end = NULL;
	unsigned long idx;

	if (strncmp(key, "action", 6) != 0)
		return -1;
	errno = 0;
	idx = strtoul(key + 6, &end, 10);
	if (errno || !end || *end || idx >= KEY_COUNT)
		return -1;
	return (int)idx;
}

static void profile_set_defaults(struct profile_config *profile)
{
	snprintf(profile->active_profile, sizeof(profile->active_profile),
		 "%s", "vibe");
	profile->sound_enabled = false;
	profile->sound_volume = AIKB_PROFILE_SOUND_VOLUME_DEFAULT;
	memcpy(profile->keymap, g_default_keymap, sizeof(profile->keymap));
	memcpy(profile->actionmap, g_default_actionmap,
	       sizeof(profile->actionmap));
}

static bool parse_bool_value(const char *value, bool *out)
{
	if (!strcmp(value, "1") || !strcmp(value, "true") ||
	    !strcmp(value, "on") || !strcmp(value, "yes")) {
		*out = true;
		return true;
	}
	if (!strcmp(value, "0") || !strcmp(value, "false") ||
	    !strcmp(value, "off") || !strcmp(value, "no")) {
		*out = false;
		return true;
	}
	return false;
}

static bool parse_percent_value(const char *value, unsigned *out)
{
	char *end = NULL;
	unsigned long v;

	errno = 0;
	v = strtoul(value, &end, 10);
	if (errno || !end || *end || v > 100ul)
		return false;
	*out = (unsigned)v;
	return true;
}

static void profile_apply_pair(struct profile_config *profile,
			       const char *key, const char *value)
{
	int key_index;
	int action_index;
	bool b;
	unsigned percent;

	key_index = profile_key_index(key);
	if (key_index >= 0) {
		struct key_binding binding;

		if (parse_key_binding_value(value, &binding))
			profile->keymap[key_index] = binding;
		return;
	}
	action_index = profile_action_index(key);
	if (action_index >= 0) {
		uint8_t bit;

		if (parse_vibe_action_value(value, &bit))
			profile->actionmap[action_index] = bit;
		return;
	}
	if (!strcmp(key, "active_profile") || !strcmp(key, "profile")) {
		if (profile_id_valid(value)) {
			snprintf(profile->active_profile,
				 sizeof(profile->active_profile), "%s", value);
		}
		return;
	}
	if (!strcmp(key, "sound_enabled") || !strcmp(key, "sound")) {
		if (parse_bool_value(value, &b))
			profile->sound_enabled = b;
		return;
	}
	if (!strcmp(key, "sound_volume") || !strcmp(key, "volume")) {
		if (parse_percent_value(value, &percent))
			profile->sound_volume = percent;
		return;
	}
}

static bool profile_load_file(struct profile_config *profile, const char *path)
{
	FILE *fp;
	char line[160];
	bool loaded = false;

	fp = fopen(path, "r");
	if (!fp)
		return false;
	while (fgets(line, sizeof(line), fp)) {
		char *p = trim_ascii(line);
		char *eq;

		if (!p[0] || p[0] == '#')
			continue;
		eq = strchr(p, '=');
		if (!eq)
			continue;
		*eq = '\0';
		profile_apply_pair(profile, trim_ascii(p), trim_ascii(eq + 1));
		loaded = true;
	}
	fclose(fp);
	return loaded;
}

static void profile_load(struct profile_config *profile)
{
	profile_set_defaults(profile);
	if (profile_load_file(profile, AIKB_PROFILE_CONFIG))
		return;
	(void)profile_load_file(profile, AIKB_PROFILE_BOOT_CONFIG);
}

static int write_all_fd(int fd, const char *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t n = write(fd, buf + off, len - off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			return -1;
		off += (size_t)n;
	}
	return 0;
}

static bool profile_save(const struct profile_config *profile)
{
	char tmp_path[128];
	char buf[1024];
	int fd;
	int dirfd;
	int n;
	size_t off = 0;
	bool ok = false;

	if (mkdir("/mnt/system/etc", 0755) != 0 && errno != EEXIST)
		return false;
	if (mkdir(AIKB_PROFILE_CONFIG_DIR, 0755) != 0 && errno != EEXIST)
		return false;
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", AIKB_PROFILE_CONFIG);
	fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return false;
	n = snprintf(buf, sizeof(buf),
		     "version=1\nactive_profile=%s\nsound_enabled=%u\nsound_volume=%u\n",
		     profile->active_profile,
		     profile->sound_enabled ? 1u : 0u, profile->sound_volume);
	if (n > 0 && n < (int)sizeof(buf)) {
		off = (size_t)n;
		for (size_t i = 0; i < KEY_COUNT; i++) {
			char binding[32];

			key_binding_format(&profile->keymap[i], binding,
					   sizeof(binding));
			n = snprintf(buf + off, sizeof(buf) - off,
				     "key%zu=%s\n", i, binding);
			if (n < 0 || (size_t)n >= sizeof(buf) - off) {
				off = sizeof(buf);
				break;
			}
			off += (size_t)n;
		}
		for (size_t i = 0; i < KEY_COUNT; i++) {
			char action[32];

			vibe_action_format(profile->actionmap[i], action,
					   sizeof(action));
			n = snprintf(buf + off, sizeof(buf) - off,
				     "action%zu=%s\n", i, action);
			if (n < 0 || (size_t)n >= sizeof(buf) - off) {
				off = sizeof(buf);
				break;
			}
			off += (size_t)n;
		}
		if (off < sizeof(buf) &&
		    write_all_fd(fd, buf, off) == 0 &&
		    fsync(fd) == 0) {
			ok = true;
		}
	}
	close(fd);
	if (!ok) {
		unlink(tmp_path);
		return false;
	}
	if (rename(tmp_path, AIKB_PROFILE_CONFIG) != 0) {
		unlink(tmp_path);
		return false;
	}
	dirfd = open(AIKB_PROFILE_CONFIG_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dirfd >= 0) {
		(void)fsync(dirfd);
		close(dirfd);
	}
	return true;
}

static int notify_hid_reload(void)
{
	DIR *dir;
	struct dirent *de;
	int count = 0;

	dir = opendir("/proc");
	if (!dir)
		return 0;
	while ((de = readdir(dir)) != NULL) {
		char path[64];
		char comm[64];
		FILE *fp;
		char *end = NULL;
		long pid;

		if (!isdigit((unsigned char)de->d_name[0]))
			continue;
		errno = 0;
		pid = strtol(de->d_name, &end, 10);
		if (errno || !end || *end || pid <= 1)
			continue;
		snprintf(path, sizeof(path), "/proc/%ld/comm", pid);
		fp = fopen(path, "r");
		if (!fp)
			continue;
		if (fgets(comm, sizeof(comm), fp)) {
			char *name = trim_ascii(comm);

			if (!strcmp(name, "aikb_hid_input") &&
			    kill((pid_t)pid, SIGHUP) == 0) {
				count++;
			}
		}
		fclose(fp);
	}
	closedir(dir);
	return count;
}

static int hex_value(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static void url_decode(char *s)
{
	char *r = s;
	char *w = s;

	while (*r) {
		if (*r == '+') {
			*w++ = ' ';
			r++;
		} else if (*r == '%' && isxdigit((unsigned char)r[1]) &&
			   isxdigit((unsigned char)r[2])) {
			int hi = hex_value(r[1]);
			int lo = hex_value(r[2]);

			*w++ = (char)((hi << 4) | lo);
			r += 3;
		} else {
			*w++ = *r++;
		}
	}
	*w = '\0';
}

static void profile_apply_form(struct profile_config *profile, char *body)
{
	char *saveptr = NULL;
	char *tok;

	for (tok = strtok_r(body, "&", &saveptr); tok;
	     tok = strtok_r(NULL, "&", &saveptr)) {
		char *eq = strchr(tok, '=');

		if (!eq)
			continue;
		*eq = '\0';
		url_decode(tok);
		url_decode(eq + 1);
		profile_apply_pair(profile, trim_ascii(tok), trim_ascii(eq + 1));
	}
}

static int append_profile_json(char *buf, size_t len,
			       const struct profile_config *profile,
			       const struct listener *listeners,
			       size_t listener_count, int reload_count)
{
	size_t off = 0;
	size_t i;
	int n;

	n = snprintf(buf + off, len - off,
		     "{\"version\":1,\"active_profile\":\"%s\",\"sound_enabled\":%u,\"sound_volume\":%u,\"reload_count\":%d,\"keys\":[",
		     profile->active_profile,
		     profile->sound_enabled ? 1u : 0u,
		     profile->sound_volume, reload_count);
	if (n < 0 || (size_t)n >= len - off)
		return -1;
	off += (size_t)n;
	for (i = 0; i < KEY_COUNT; i++) {
		char binding[32];

		key_binding_format(&profile->keymap[i], binding,
				   sizeof(binding));
		n = snprintf(buf + off, len - off, "%s\"%s\"",
			     i ? "," : "", binding);
		if (n < 0 || (size_t)n >= len - off)
			return -1;
		off += (size_t)n;
	}
	n = snprintf(buf + off, len - off, "],\"actions\":[");
	if (n < 0 || (size_t)n >= len - off)
		return -1;
	off += (size_t)n;
	for (i = 0; i < KEY_COUNT; i++) {
		char action[32];

		vibe_action_format(profile->actionmap[i], action,
				   sizeof(action));
		n = snprintf(buf + off, len - off, "%s\"%s\"",
			     i ? "," : "", action);
		if (n < 0 || (size_t)n >= len - off)
			return -1;
		off += (size_t)n;
	}
	n = snprintf(buf + off, len - off, "],\"urls\":[");
	if (n < 0 || (size_t)n >= len - off)
		return -1;
	off += (size_t)n;
	for (i = 0; i < listener_count; i++) {
		n = snprintf(buf + off, len - off, "%s\"http://%s/\"",
			     i ? "," : "", listeners[i].ip);
		if (n < 0 || (size_t)n >= len - off)
			return -1;
		off += (size_t)n;
	}
	n = snprintf(buf + off, len - off, "]}");
	if (n < 0 || (size_t)n >= len - off)
		return -1;
	return (int)(off + (size_t)n);
}

static int send_all(int fd, const char *buf, size_t len)
{
	return write_all_fd(fd, buf, len);
}

static void send_response(int fd, int status, const char *reason,
			  const char *ctype, const char *body)
{
	char header[512];
	size_t body_len = strlen(body);
	int n;

	n = snprintf(header, sizeof(header),
		     "HTTP/1.1 %d %s\r\n"
		     "Content-Type: %s\r\n"
		     "Content-Length: %zu\r\n"
		     "Cache-Control: no-store\r\n"
		     "Connection: close\r\n"
		     "\r\n",
		     status, reason, ctype, body_len);
	if (n > 0 && n < (int)sizeof(header))
		(void)send_all(fd, header, (size_t)n);
	(void)send_all(fd, body, body_len);
}

static void send_text(int fd, int status, const char *reason, const char *body)
{
	send_response(fd, status, reason, "text/plain; charset=utf-8", body);
}

static bool startswith_ci(const char *s, const char *prefix)
{
	while (*prefix) {
		if (tolower((unsigned char)*s) !=
		    tolower((unsigned char)*prefix))
			return false;
		s++;
		prefix++;
	}
	return true;
}

static int parse_content_length(const char *headers)
{
	const char *p = headers;

	while (*p) {
		const char *line = p;
		const char *next = strstr(p, "\r\n");
		size_t line_len;

		if (!next)
			break;
		line_len = (size_t)(next - line);
		if (line_len == 0)
			break;
		if (startswith_ci(line, "Content-Length:")) {
			const char *v = line + strlen("Content-Length:");

			while (*v == ' ' || *v == '\t')
				v++;
			return atoi(v);
		}
		p = next + 2;
	}
	return 0;
}

static void handle_client(int fd, const struct listener *listeners,
			  size_t listener_count)
{
	char req[REQ_BUF_SIZE + 1];
	char method[12];
	char path[192];
	char version[16];
	char *header_end;
	char *body;
	ssize_t total = 0;
	int content_length;
	int body_have;

	while (total < REQ_BUF_SIZE) {
		ssize_t n = recv(fd, req + total, REQ_BUF_SIZE - (size_t)total, 0);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		if (n == 0)
			break;
		total += n;
		req[total] = '\0';
		header_end = strstr(req, "\r\n\r\n");
		if (!header_end)
			continue;
		content_length = parse_content_length(req);
		if (content_length < 0 || content_length > BODY_LIMIT) {
			send_text(fd, 413, "Payload Too Large", "payload too large\n");
			return;
		}
		body = header_end + 4;
		body_have = (int)(total - (body - req));
		if (body_have >= content_length)
			break;
	}
	req[total] = '\0';
	header_end = strstr(req, "\r\n\r\n");
	if (!header_end) {
		send_text(fd, 400, "Bad Request", "bad request\n");
		return;
	}
	if (sscanf(req, "%11s %191s %15s", method, path, version) != 3) {
		send_text(fd, 400, "Bad Request", "bad request\n");
		return;
	}
	body = header_end + 4;
	content_length = parse_content_length(req);
	if (content_length < 0 || content_length > BODY_LIMIT) {
		send_text(fd, 413, "Payload Too Large", "payload too large\n");
		return;
	}
	body[content_length] = '\0';
	{
		char *query = strchr(path, '?');

		if (query)
			*query = '\0';
	}

	if (!strcmp(method, "OPTIONS")) {
		send_response(fd, 204, "No Content", "text/plain; charset=utf-8", "");
		return;
	}
	if (!strcmp(method, "GET") &&
	    (!strcmp(path, "/") || !strcmp(path, "/index.html"))) {
		send_response(fd, 200, "OK", "text/html; charset=utf-8",
			      k_index_html);
		return;
	}
	if (!strcmp(method, "GET") && !strcmp(path, "/api/profile")) {
		struct profile_config profile;
		char json[1536];

		profile_load(&profile);
		if (append_profile_json(json, sizeof(json), &profile, listeners,
					listener_count, 0) < 0) {
			send_text(fd, 500, "Internal Server Error", "json too large\n");
			return;
		}
		send_response(fd, 200, "OK", "application/json; charset=utf-8",
			      json);
		return;
	}
	if (!strcmp(method, "POST") && !strcmp(path, "/api/profile")) {
		struct profile_config profile;
		char json[1536];
		int reload_count;

		profile_load(&profile);
		profile_apply_form(&profile, body);
		if (!profile_save(&profile)) {
			send_text(fd, 500, "Internal Server Error", "save failed\n");
			return;
		}
		reload_count = notify_hid_reload();
		if (append_profile_json(json, sizeof(json), &profile, listeners,
					listener_count, reload_count) < 0) {
			send_text(fd, 500, "Internal Server Error", "json too large\n");
			return;
		}
		send_response(fd, 200, "OK", "application/json; charset=utf-8",
			      json);
		return;
	}
	if (!strcmp(method, "POST") && !strcmp(path, "/api/reset")) {
		struct profile_config profile;
		char json[1536];
		int reload_count;

		profile_set_defaults(&profile);
		if (!profile_save(&profile)) {
			send_text(fd, 500, "Internal Server Error", "save failed\n");
			return;
		}
		reload_count = notify_hid_reload();
		if (append_profile_json(json, sizeof(json), &profile, listeners,
					listener_count, reload_count) < 0) {
			send_text(fd, 500, "Internal Server Error", "json too large\n");
			return;
		}
		send_response(fd, 200, "OK", "application/json; charset=utf-8",
			      json);
		return;
	}
	send_text(fd, 404, "Not Found", "not found\n");
}

static int open_listener(const char *ifname, const char *ip, unsigned port)
{
	int fd;
	int yes = 1;
	struct sockaddr_in addr;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
		close(fd);
		return -1;
	}
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		fprintf(stderr, "aikb_config_webd: bind %s:%u (%s) failed: %s\n",
			ip, port, ifname, strerror(errno));
		close(fd);
		return -1;
	}
	if (listen(fd, 8) != 0) {
		fprintf(stderr, "aikb_config_webd: listen %s:%u failed: %s\n",
			ip, port, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

static bool listener_exists(const struct listener *listeners, size_t count,
			    const char *ip)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (listeners[i].fd >= 0 && !strcmp(listeners[i].ip, ip))
			return true;
	}
	return false;
}

static size_t add_usb_listeners(struct listener *listeners, size_t count,
				size_t max, unsigned port)
{
	struct ifaddrs *ifaddr = NULL;
	struct ifaddrs *ifa;

	if (getifaddrs(&ifaddr) != 0)
		return count;
	for (ifa = ifaddr; ifa && count < max; ifa = ifa->ifa_next) {
		struct sockaddr_in *sin;
		char ip[INET_ADDRSTRLEN];
		int fd;

		if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
			continue;
		if (strncmp(ifa->ifa_name, "usb", 3) != 0)
			continue;
		if (!(ifa->ifa_flags & IFF_UP))
			continue;
		sin = (struct sockaddr_in *)ifa->ifa_addr;
		if (!inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip)))
			continue;
		if (!strcmp(ip, "0.0.0.0"))
			continue;
		if (listener_exists(listeners, count, ip))
			continue;
		fd = open_listener(ifa->ifa_name, ip, port);
		if (fd < 0)
			continue;
		listeners[count].fd = fd;
		snprintf(listeners[count].ifname, sizeof(listeners[count].ifname),
			 "%s", ifa->ifa_name);
		snprintf(listeners[count].ip, sizeof(listeners[count].ip),
			 "%s", ip);
		fprintf(stderr, "aikb_config_webd: http://%s/ on %s\n",
			listeners[count].ip, listeners[count].ifname);
		count++;
	}
	freeifaddrs(ifaddr);
	return count;
}

static void close_listeners(struct listener *listeners, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (listeners[i].fd >= 0)
			close(listeners[i].fd);
		listeners[i].fd = -1;
	}
}

static unsigned parse_port(void)
{
	const char *env = getenv("AIKB_CONFIG_WEB_PORT");
	char *end = NULL;
	unsigned long port;

	if (!env || !env[0])
		return DEFAULT_PORT;
	errno = 0;
	port = strtoul(env, &end, 10);
	if (errno || !end || *end || port == 0 || port > 65535ul)
		return DEFAULT_PORT;
	return (unsigned)port;
}

int main(void)
{
	struct listener listeners[MAX_LISTENERS];
	size_t listener_count = 0;
	unsigned port = parse_port();
	size_t i;

	for (i = 0; i < MAX_LISTENERS; i++)
		listeners[i].fd = -1;
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	while (!g_stop) {
		fd_set rfds;
		int maxfd = -1;
		int rc;

		listener_count = add_usb_listeners(listeners, listener_count,
						   MAX_LISTENERS, port);
		if (listener_count == 0) {
			fprintf(stderr,
				"aikb_config_webd: no usb IPv4 address, retry in 2s\n");
			sleep(2);
			continue;
		}
		while (!g_stop) {
			struct timeval tv;

			FD_ZERO(&rfds);
			maxfd = -1;
			for (i = 0; i < listener_count; i++) {
				FD_SET(listeners[i].fd, &rfds);
				if (listeners[i].fd > maxfd)
					maxfd = listeners[i].fd;
			}
			tv.tv_sec = 2;
			tv.tv_usec = 0;
			rc = select(maxfd + 1, &rfds, NULL, NULL, &tv);
			if (rc < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			if (rc == 0) {
				listener_count =
					add_usb_listeners(listeners, listener_count,
							  MAX_LISTENERS, port);
				continue;
			}
			for (i = 0; i < listener_count; i++) {
				if (FD_ISSET(listeners[i].fd, &rfds)) {
					int client = accept(listeners[i].fd, NULL, NULL);

					if (client >= 0) {
						handle_client(client, listeners,
							      listener_count);
						close(client);
					}
				}
			}
		}
		close_listeners(listeners, listener_count);
	}
	close_listeners(listeners, MAX_LISTENERS);
	return 0;
}
