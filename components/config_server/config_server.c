/**
 * Always-on HTTP portal: status dashboard, manual control, settings and web OTA.
 * Runs in both STA and AP mode so the device is never unreachable.
 */

#include "config_server.h"
#include "config_manager.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "ha_discovery.h"
#include "somfy_rts.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "config_server";

/* Implemented in app_main.c */
extern void somfy_enqueue_command(uint8_t button, const char *label);

/* ── Shared page chrome ───────────────────────────────────────────────────── */

static const char CSS[] =
    "body{font-family:sans-serif;margin:0;padding:16px;background:#f4f4f4;}"
    ".box{background:#fff;border-radius:8px;padding:20px;margin:14px 0;"
         "box-shadow:0 2px 6px rgba(0,0,0,.12);}"
    ".grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;}"
    "@media(max-width:640px){.grid{grid-template-columns:1fr}}"
    "h1{margin:0 0 4px;font-size:1.4em;color:#222;}"
    "h2{font-size:1em;color:#555;margin:0 0 10px;text-transform:uppercase;"
       "letter-spacing:.05em;border-bottom:2px solid #eee;padding-bottom:6px;}"
    "label{display:block;font-weight:bold;margin:10px 0 3px;color:#333;}"
    "input,select{width:100%;padding:8px;border:1px solid #ccc;border-radius:4px;"
          "box-sizing:border-box;font-size:1em;}"
    "button{background:#1a73e8;color:#fff;padding:10px 20px;border:none;"
            "border-radius:4px;cursor:pointer;font-size:1em;margin-top:14px;}"
    "button:hover{background:#1558b0;}"
    "button.r{background:#d93025;} button.r:hover{background:#b52a1e;}"
    "button.big{font-size:1.3em;padding:18px 10px;width:100%;margin:6px 0;}"
    /* Transient states for the RTS buttons — set by JS, cleared on a timer */
    "button.busy,button.busy:hover{background:#e8a000;}"
    "button.done,button.done:hover{background:#188038;}"
    "button.fail,button.fail:hover{background:#d93025;}"
    "nav a{display:inline-block;margin:0 8px 12px 0;padding:8px 14px;"
          "background:#1a73e8;color:#fff;text-decoration:none;border-radius:4px;}"
    "nav a:hover{background:#1558b0;}"
    ".ok{color:#188038;font-weight:bold;} .warn{color:#e8a000;font-weight:bold;}"
    ".err{color:#d93025;font-weight:bold;}"
    ".big{font-size:1.6em;font-weight:bold;color:#1a1a1a;}"
    ".unit{font-size:.8em;color:#888;margin-left:2px;}"
    "table{width:100%;border-collapse:collapse;}"
    "td,th{padding:5px 8px;text-align:left;border-bottom:1px solid #f0f0f0;font-size:.95em;}"
    "th{color:#888;font-weight:normal;width:55%;}"
    ".hint{color:#888;font-size:.9em;}"
    ".refresh{font-size:.8em;color:#aaa;margin-left:8px;}";

#define NAV \
    "<nav><a href='/'>Status</a><a href='/control'>Control</a>" \
    "<a href='/config'>Settings</a><a href='/ota'>OTA Update</a></nav>"

/* ── Form helpers ─────────────────────────────────────────────────────────── */

static void url_decode(char *dst, const char *src, size_t len)
{
    size_t i = 0;
    while (*src && i < len - 1) {
        if (*src == '%' && src[1] && src[2]) {
            char a = src[1], b = src[2];
            a = (a >= 'A') ? ((a & 0xDF) - 'A' + 10) : (a - '0');
            b = (b >= 'A') ? ((b & 0xDF) - 'A' + 10) : (b - '0');
            dst[i++] = 16 * a + b;
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' '; src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

static void get_field(const char *body, const char *key, char *out, size_t out_len)
{
    char search[64];
    snprintf(search, sizeof(search), "%s=", key);
    const char *p = strstr(body, search);
    if (!p) { out[0] = '\0'; return; }
    p += strlen(search);
    const char *end = strchr(p, '&');
    size_t vlen = end ? (size_t)(end - p) : strlen(p);
    if (vlen >= out_len) vlen = out_len - 1;
    char tmp[256] = {0};
    if (vlen < sizeof(tmp)) { memcpy(tmp, p, vlen); tmp[vlen] = '\0'; }
    url_decode(out, tmp, out_len);
}

static int read_body(httpd_req_t *req, char *buf, size_t max_len)
{
    int total = 0, remaining = (int)req->content_len;
    if (remaining <= 0 || (size_t)remaining >= max_len) remaining = (int)max_len - 1;
    while (total < remaining) {
        int r = httpd_req_recv(req, buf + total, remaining - total);
        if (r <= 0) break;
        total += r;
    }
    buf[total] = '\0';
    return total;
}

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

static esp_err_t send_ok(httpd_req_t *req, const char *msg, const char *back)
{
    char buf[320];
    snprintf(buf, sizeof(buf),
        "<!DOCTYPE html><html><head><meta charset=UTF-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>OK</title></head>"
        "<body style='font-family:sans-serif;padding:20px'>"
        "<h2>&#10003; %s</h2><p><a href='%s'>Back</a></p></body></html>",
        msg, back);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── GET / — status dashboard ─────────────────────────────────────────────── */

static esp_err_t root_get(httpd_req_t *req)
{
    char ip[16], mac[18];
    wifi_manager_get_ip(ip, sizeof(ip));
    wifi_manager_get_mac(mac, sizeof(mac));
    bool ap = wifi_manager_is_ap_mode();

    const esp_app_desc_t *app = esp_app_get_description();
    int64_t uptime_s = esp_timer_get_time() / 1000000;

    char *buf = malloc(4096);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }

    int n = 0;
    n += snprintf(buf + n, 4096 - n,
        "<!DOCTYPE html><html><head><meta charset=UTF-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<meta http-equiv=refresh content=10>"
        "<title>Somfy Markise</title><style>%s</style></head><body>"
        "<h1>Terrasse Markise <span class=refresh>(auto-refresh 10s)</span></h1>"
        NAV,
        CSS);

    /* What the blind was last told to learn, versus what we transmit now. They
     * only diverge if the address was changed after pairing. */
    char paired[96];
    if (g_config.paired_addr == 0) {
        snprintf(paired, sizeof(paired),
                 "<span class=warn>never paired from this device</span>");
    } else if (g_config.paired_addr == somfy_rts_get_remote_addr()) {
        snprintf(paired, sizeof(paired), "0x%06lX <span class=ok>&#10003;</span>",
                 (unsigned long)g_config.paired_addr);
    } else {
        snprintf(paired, sizeof(paired),
                 "<span class=err>0x%06lX &mdash; differs, re-pair</span>",
                 (unsigned long)g_config.paired_addr);
    }

    n += snprintf(buf + n, 4096 - n,
        "<div class=grid>"
        "<div class=box><h2>Somfy RTS</h2><table>"
        "<tr><th>Remote address (transmitting)</th><td>0x%06lX</td></tr>"
        "<tr><th>Last paired with</th><td>%s</td></tr>"
        "<tr><th>Next rolling code</th><td><span class=big>%lu</span></td></tr>"
        "<tr><th>Transmitter GPIO</th><td>%u</td></tr>"
        "<tr><th>Cover type</th><td>%s</td></tr>"
        "</table></div>",
        /* From the transmitter, not the config, so it is what actually goes out */
        (unsigned long)somfy_rts_get_remote_addr(),
        paired,
        (unsigned long)somfy_rts_get_rolling_code(),
        g_config.tx_gpio,
        g_config.cover_open_extends ? "Awning — OPEN extends (DOWN)"
                                    : "Shutter — OPEN retracts (UP)");

    n += snprintf(buf + n, 4096 - n,
        "<div class=box><h2>Network</h2><table>"
        "<tr><th>WiFi mode</th><td>%s</td></tr>"
        "<tr><th>IP address</th><td>%s</td></tr>"
        "<tr><th>MAC address</th><td>%s</td></tr>"
        "<tr><th>Signal</th><td>%d dBm</td></tr>"
        "<tr><th>MQTT broker</th><td>%s</td></tr>"
        "<tr><th>MQTT state</th><td>%s</td></tr>"
        "</table></div>",
        ap ? "Access Point (provisioning)" : "Station",
        ip[0] ? ip : "-",
        mac,
        wifi_manager_get_rssi(),
        g_config.mqtt_url[0] ? g_config.mqtt_url : "<i>not configured</i>",
        mqtt_manager_is_connected() ? "<span class=ok>connected</span>"
                                    : "<span class=err>disconnected</span>");

    n += snprintf(buf + n, 4096 - n,
        "<div class=box><h2>Device</h2><table>"
        "<tr><th>Hostname</th><td>%s.local</td></tr>"
        "<tr><th>Firmware</th><td>%s (%s)</td></tr>"
        "<tr><th>Uptime</th><td>%lud %02luh %02lum %02lus</td></tr>"
        "<tr><th>Free heap</th><td>%u B</td></tr>"
        "<tr><th>Free heap minimum</th><td>%u B</td></tr>"
        "</table></div>"
        "</div>",
        g_config.hostname,
        app->version, app->date,
        (unsigned long)(uptime_s / 86400),
        (unsigned long)((uptime_s % 86400) / 3600),
        (unsigned long)((uptime_s % 3600) / 60),
        (unsigned long)(uptime_s % 60),
        (unsigned)esp_get_free_heap_size(),
        (unsigned)esp_get_minimum_free_heap_size());

    if (ap) {
        n += snprintf(buf + n, 4096 - n,
            "<div class=box style='border-left:4px solid #e8a000'>"
            "<b>&#9888; Not connected to WiFi.</b> Open "
            "<a href='/config'>/config</a> to set credentials.</div>");
    }

    snprintf(buf + n, 4096 - n, "</body></html>");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    free(buf);
    return ESP_OK;
}

/* ── GET /control ─────────────────────────────────────────────────────────── */

static esp_err_t control_get(httpd_req_t *req)
{
    /* CSS ~1.6 kB + markup ~1.5 kB + the inline script ~0.9 kB */
    enum { CONTROL_PAGE_SZ = 6144 };

    char *buf = malloc(CONTROL_PAGE_SZ);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }

    snprintf(buf, CONTROL_PAGE_SZ,
        "<!DOCTYPE html><html><head><meta charset=UTF-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Markise Control</title><style>%s</style></head><body>"
        "<h1>Markise Control</h1>"
        NAV

        "<div class=box><h2>Move</h2>"
        "<button type=button class=big onclick=\"cmd('u',this)\">&#9650;&nbsp; Up (retract)</button>"
        "<button type=button class=big onclick=\"cmd('s',this)\">&#9632;&nbsp; Stop</button>"
        "<button type=button class=big onclick=\"cmd('d',this)\">&#9660;&nbsp; Down (extend)</button>"
        "</div>"

        "<div class=box><h2>Pairing</h2>"
        "<p class=hint>Long-press the PROG button on your real remote until the "
        "blind jogs, then press this within a few seconds to pair this emulated "
        "remote (address <b>0x%06lX</b>).</p>"
        "<button type=button onclick=\"cmd('p',this)\">Send PROG</button>"
        "</div>"

        "<div class=box><h2>Rolling code</h2>"
        "<p class=hint>Next code to be sent: <b id=rc>%lu</b>. The blind only "
        "accepts codes ahead of the last one it saw &mdash; if commands stop "
        "working, raise it on the <a href='/config'>Settings</a> page or "
        "re-pair.</p>"
        "</div>"

        /* Commands go out over fetch() so the page never navigates away; the
         * button colour is the only feedback, and it is disabled meanwhile so a
         * double-press cannot queue a second command mid-transmission. */
        "<script>"
        "function cmd(b,el){"
          "var t=el.className;"
          "el.className=t+' busy';el.disabled=true;"
          "fetch('/control/cmd',{method:'POST',"
            "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
            "body:'btn='+b})"
          ".then(function(r){"
            "el.className=t+(r.ok?' done':' fail');"
            "if(r.ok)setTimeout(rc,1500);"
          "})"
          ".catch(function(){el.className=t+' fail';})"
          ".then(function(){setTimeout(function(){"
            "el.className=t;el.disabled=false;},1500);});"
        "}"
        "function rc(){"
          "fetch('/control/rc').then(function(r){return r.text();})"
          ".then(function(t){document.getElementById('rc').textContent=t;})"
          ".catch(function(){});"
        "}"
        "</script>"

        "</body></html>",
        CSS,
        (unsigned long)somfy_rts_get_remote_addr(),
        (unsigned long)somfy_rts_get_rolling_code());

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    free(buf);
    return ESP_OK;
}

static esp_err_t control_cmd_post(httpd_req_t *req)
{
    char body[64];
    read_body(req, body, sizeof(body));

    char btn[8] = {0};
    get_field(body, "btn", btn, sizeof(btn));

    switch (btn[0]) {
        case 'u': somfy_enqueue_command(SOMFY_BTN_UP,   "up");   break;
        case 'd': somfy_enqueue_command(SOMFY_BTN_DOWN, "down"); break;
        case 's': somfy_enqueue_command(SOMFY_BTN_STOP, "stop"); break;
        case 'p': somfy_enqueue_command(SOMFY_BTN_PROG, "prog"); break;
        default:
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown command");
            return ESP_FAIL;
    }

    /* Answered by fetch(), not a browser navigation — plain text keeps it small
     * and lets the JS distinguish success from failure by status alone. */
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "queued");
    return ESP_OK;
}

/* Current rolling code as bare text, polled by the control page after a send */
static esp_err_t control_rc_get(httpd_req_t *req)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)somfy_rts_get_rolling_code());
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* ── GET /config ──────────────────────────────────────────────────────────── */

static esp_err_t config_get(httpd_req_t *req)
{
    /* CSS ~1.6 kB + five forms with their hint paragraphs */
    enum { CONFIG_PAGE_SZ = 8192 };

    char *buf = malloc(CONFIG_PAGE_SZ);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }

    snprintf(buf, CONFIG_PAGE_SZ,
        "<!DOCTYPE html><html><head><meta charset=UTF-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Markise Settings</title><style>%s</style></head><body>"
        "<h1>Markise Settings</h1>"
        NAV

        "<div class=box><h2>Device</h2>"
        "<form method=POST action=/config/hostname>"
        "<label>Hostname</label>"
        "<input type=text name=hostname value='%s' maxlength=31 placeholder='markise-esp'>"
        "<p class=hint>Reachable at hostname.local via mDNS. Reboots on save.</p>"
        "<button type=submit>Save Hostname</button>"
        "</form></div>"

        "<div class=box><h2>WiFi</h2>"
        "<form method=POST action=/config/wifi>"
        "<label>SSID</label>"
        "<input type=text name=ssid value='%s' maxlength=32 placeholder='Network name'>"
        "<label>Password</label>"
        "<input type=password name=pass maxlength=63 placeholder='Leave empty to keep current'>"
        "<button type=submit>Save WiFi &amp; Restart</button>"
        "</form></div>"

        "<div class=box><h2>MQTT</h2>"
        "<form method=POST action=/config/mqtt>"
        "<label>Broker URL</label>"
        "<input type=text name=url value='%s' maxlength=127 placeholder='mqtt://192.168.1.x:1883'>"
        "<label>Username (optional)</label>"
        "<input type=text name=user value='%s' maxlength=31>"
        "<label>Password (optional)</label>"
        "<input type=password name=pass maxlength=31 placeholder='Leave empty to keep current'>"
        "<p class=hint>Takes effect after reboot.</p>"
        "<button type=submit>Save MQTT</button>"
        "</form></div>"

        "<div class=box><h2>Somfy RTS</h2>"
        "<form method=POST action=/config/somfy>"
        "<label>Remote address (hex, 24 bit)</label>"
        "<input type=text name=remote value='%06lX' maxlength=8>"
        "<p class=hint>Six hex digits, with or without a leading <code>0x</code>. "
        "Takes effect immediately &mdash; the blind will ignore the device until "
        "you pair it again from the Control page. Default 121309. The blind "
        "learns whatever address is transmitted; nothing is read back from it, "
        "so the Status page shows the address last sent with PROG for comparison.</p>"
        "<label>Transmitter GPIO</label>"
        "<input type=number name=tx_gpio value='%u' min=0 max=33>"
        "<label>Diagnostics publish interval (s)</label>"
        "<input type=number name=pub_ivl value='%lu' min=5 max=3600>"
        "<p class=hint>The transmitter GPIO takes effect after a reboot.</p>"
        "<button type=submit>Save Somfy Settings</button>"
        "</form></div>"

        /* Its own box: buried at the foot of the Somfy form behind two hint
         * paragraphs, this control was effectively invisible. Posts to the same
         * handler, which keeps any field that is absent from the body. */
        "<div class=box><h2>Home Assistant Cover</h2>"
        "<form method=POST action=/config/somfy>"
        "<label>Cover type</label>"
        "<select name=cover_ext>"
        "<option value=0%s>Shutter &mdash; OPEN retracts (sends UP)</option>"
        "<option value=1%s>Awning &mdash; OPEN extends (sends DOWN)</option>"
        "</select>"
        "<p class=hint>Sets the Home Assistant device class and the button "
        "mapping together, so the reported state always agrees with the icon. "
        "<b>Shutter</b> reports <code>open</code> when retracted and gets the "
        "window-shutter icons; <b>Awning</b> reports <code>open</code> when "
        "deployed and gets the generic cover icon. Saving republishes discovery "
        "immediately. Affects the <b>cover</b> entity only &mdash; the Up/Down "
        "buttons always send their own direction.</p>"
        "<button type=submit>Save Cover Type</button>"
        "</form></div>"

        "<div class=box style='border-left:4px solid #e8a000'>"
        "<h2>Rolling code</h2>"
        "<form method=POST action=/config/rolling>"
        "<label>Next rolling code</label>"
        "<input type=number name=rolling value='%lu' min=1 max=65535>"
        "<p class=hint><b>Only change this deliberately.</b> The blind accepts a "
        "code only if it is ahead of the last one it received. After migrating "
        "from the Arduino firmware, set this to the value that firmware last "
        "printed as <i>Current rolling code</i>, plus a small margin. Setting it "
        "too low means the blind ignores every command until you re-pair.</p>"
        "<button type=submit class=r>Set Rolling Code</button>"
        "</form></div>"

        "<div class=box><h2>Power</h2>"
        "<form method=POST action=/reboot>"
        "<button type=submit class=r>Reboot</button>"
        "</form></div>"

        "</body></html>",
        CSS,
        g_config.hostname,
        g_config.wifi_ssid,
        g_config.mqtt_url,
        g_config.mqtt_user,
        (unsigned long)somfy_rts_get_remote_addr(),
        g_config.tx_gpio,
        (unsigned long)g_config.pub_interval,
        /* Order matches the <option> order above: value=0 (shutter) first */
        g_config.cover_open_extends ? "" : " selected",
        g_config.cover_open_extends ? " selected" : "",
        (unsigned long)somfy_rts_get_rolling_code());

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    free(buf);
    return ESP_OK;
}

/* ── Settings POST handlers ───────────────────────────────────────────────── */

static esp_err_t config_hostname_post(httpd_req_t *req)
{
    char body[128];
    read_body(req, body, sizeof(body));

    char hostname[32] = {0};
    get_field(body, "hostname", hostname, sizeof(hostname));
    if (hostname[0] != '\0') config_manager_save_hostname(hostname);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req,
        "<!DOCTYPE html><html><head><meta charset=UTF-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Saved</title></head><body style='font-family:sans-serif;padding:20px'>"
        "<h2>&#10003; Hostname saved</h2>"
        "<p>Rebooting now. The device will come back at <b>hostname.local</b>.</p>"
        "</body></html>", HTTPD_RESP_USE_STRLEN);

    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t config_wifi_post(httpd_req_t *req)
{
    char body[512];
    read_body(req, body, sizeof(body));

    char ssid[64] = {0}, pass[64] = {0};
    get_field(body, "ssid", ssid, sizeof(ssid));
    get_field(body, "pass", pass, sizeof(pass));

    /* Empty password field means "keep the stored one" */
    if (pass[0] == '\0') strncpy(pass, g_config.wifi_pass, sizeof(pass) - 1);

    config_manager_save_wifi(ssid, pass);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req,
        "<!DOCTYPE html><html><head><meta charset=UTF-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Saved</title></head><body style='font-family:sans-serif;padding:20px'>"
        "<h2>&#10003; WiFi credentials saved</h2>"
        "<p>Rebooting now. Reconnect to your network and find the device at its "
        "new IP.</p></body></html>", HTTPD_RESP_USE_STRLEN);

    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t config_mqtt_post(httpd_req_t *req)
{
    char body[512];
    read_body(req, body, sizeof(body));

    char url[128] = {0}, user[32] = {0}, pass[32] = {0};
    get_field(body, "url",  url,  sizeof(url));
    get_field(body, "user", user, sizeof(user));
    get_field(body, "pass", pass, sizeof(pass));

    if (pass[0] == '\0') strncpy(pass, g_config.mqtt_pass, sizeof(pass) - 1);

    config_manager_save_mqtt(url, user, pass);
    return send_ok(req, "MQTT settings saved — reboot to apply", "/config");
}

static esp_err_t config_somfy_post(httpd_req_t *req)
{
    char body[256];
    read_body(req, body, sizeof(body));

    char s_remote[16] = {0}, s_gpio[8] = {0}, s_pub[8] = {0}, s_cover[8] = {0};
    get_field(body, "remote",    s_remote, sizeof(s_remote));
    get_field(body, "tx_gpio",   s_gpio,   sizeof(s_gpio));
    get_field(body, "pub_ivl",   s_pub,    sizeof(s_pub));
    get_field(body, "cover_ext", s_cover,  sizeof(s_cover));

    uint32_t remote = s_remote[0] ? (uint32_t)strtoul(s_remote, NULL, 16)
                                  : g_config.remote_addr;
    uint8_t  gpio   = s_gpio[0] ? (uint8_t)atoi(s_gpio) : g_config.tx_gpio;
    uint32_t pub    = s_pub[0]  ? (uint32_t)atoi(s_pub) : g_config.pub_interval;
    bool     cover  = s_cover[0] ? (atoi(s_cover) != 0) : g_config.cover_open_extends;

    bool cover_changed = (cover != g_config.cover_open_extends);

    remote &= 0xFFFFFF;
    if (remote == 0) remote = g_config.remote_addr;
    /* Output-capable GPIOs on the classic ESP32 stop at 33 */
    if (gpio > 33) gpio = 33;
    if (pub < 5)    pub = 5;
    if (pub > 3600) pub = 3600;

    esp_err_t err = config_manager_save_somfy(remote, gpio, pub, cover);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving somfy config failed: %s", esp_err_to_name(err));
        return send_ok(req, "Save failed — settings unchanged", "/config");
    }

    /* Push the address into the transmitter straight away. Deferring it to a
     * reboot would leave the pages showing an address the radio is not actually
     * sending, which is exactly the mismatch that makes pairing confusing. */
    somfy_rts_set_remote_addr(remote);

    /* Cover type selects the Home Assistant device class, which only ships in
     * the discovery payload. Without republishing here the setting would appear
     * to do nothing until the next MQTT reconnect. */
    if (cover_changed && mqtt_manager_is_connected()) {
        ESP_LOGI(TAG, "cover type changed — republishing discovery");
        ha_discovery_publish();
    }

    return send_ok(req, cover_changed
                        ? "Saved — Home Assistant updated with the new cover type"
                        : "Somfy settings saved",
                   "/config");
}

static esp_err_t config_rolling_post(httpd_req_t *req)
{
    char body[64];
    read_body(req, body, sizeof(body));

    char s_code[16] = {0};
    get_field(body, "rolling", s_code, sizeof(s_code));
    if (s_code[0] == '\0') return send_ok(req, "No value given", "/config");

    long code = strtol(s_code, NULL, 10);
    if (code < 1)     code = 1;
    if (code > 65535) code = 65535;

    config_manager_save_rolling_code((uint32_t)code);
    ESP_LOGW(TAG, "rolling code manually set to %ld", code);
    return send_ok(req, "Rolling code updated", "/config");
}

/* ── GET /ota ─────────────────────────────────────────────────────────────── */

static esp_err_t ota_get(httpd_req_t *req)
{
    /* JS is inlined so the page has no external dependencies */
    static const char PAGE[] =
        "<!DOCTYPE html><html><head>"
        "<meta charset=UTF-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Markise OTA Update</title>"
        "<style>"
        "body{font-family:sans-serif;margin:0;padding:16px;background:#f4f4f4;}"
        ".box{background:#fff;border-radius:8px;padding:20px;margin:14px 0;"
             "box-shadow:0 2px 6px rgba(0,0,0,.12);}"
        "h1{margin:0 0 4px;font-size:1.4em;color:#222;}"
        "h2{font-size:1em;color:#555;margin:0 0 10px;text-transform:uppercase;"
           "letter-spacing:.05em;border-bottom:2px solid #eee;padding-bottom:6px;}"
        "label{display:block;font-weight:bold;margin:10px 0 3px;color:#333;}"
        "input[type=file]{width:100%;padding:8px;border:1px solid #ccc;border-radius:4px;"
                         "box-sizing:border-box;font-size:1em;background:#fff;}"
        "button{background:#1a73e8;color:#fff;padding:10px 20px;border:none;"
                "border-radius:4px;cursor:pointer;font-size:1em;margin-top:14px;}"
        "button:hover{background:#1558b0;}"
        "button:disabled{background:#aaa;cursor:default;}"
        "nav a{display:inline-block;margin:0 8px 12px 0;padding:8px 14px;"
              "background:#1a73e8;color:#fff;text-decoration:none;border-radius:4px;}"
        "nav a:hover{background:#1558b0;}"
        "progress{width:100%;height:20px;margin-top:12px;}"
        "#status{margin-top:8px;font-weight:bold;}"
        ".ok{color:#188038;} .err{color:#d93025;} .info{color:#555;}"
        "</style></head><body>"
        "<h1>Markise OTA Update</h1>"
        NAV
        "<div class=box><h2>Flash Firmware</h2>"
        "<p class=info>Select the <code>somfy2mqtt.bin</code> built for this "
        "device. It reboots automatically after a successful flash.</p>"
        "<label>Firmware file</label>"
        "<input type=file id=fw accept=.bin>"
        "<button id=btn onclick=doUpload()>Flash Firmware</button>"
        "<progress id=bar value=0 max=100 style='display:none'></progress>"
        "<div id=status></div>"
        "</div>"
        "<script>"
        "function doUpload(){"
          "var f=document.getElementById('fw').files[0];"
          "if(!f){alert('No file selected');return;}"
          "var btn=document.getElementById('btn');"
          "var bar=document.getElementById('bar');"
          "var st=document.getElementById('status');"
          "btn.disabled=true;bar.style.display='';bar.value=0;"
          "st.textContent='Uploading...';st.className='info';"
          "var xhr=new XMLHttpRequest();"
          "xhr.open('POST','/ota/upload');"
          "xhr.setRequestHeader('Content-Type','application/octet-stream');"
          "xhr.upload.onprogress=function(e){"
            "if(e.lengthComputable){"
              "var p=Math.round(e.loaded/e.total*100);"
              "bar.value=p;"
              "st.textContent='Uploading... '+p+'% ('+Math.round(e.loaded/1024)+'/'+"
                "Math.round(e.total/1024)+' KB)';"
            "}"
          "};"
          "xhr.onload=function(){"
            "if(xhr.status===200){"
              "bar.value=100;st.className='ok';"
              "st.textContent='Flash successful! Device is rebooting...';"
            "}else{"
              "st.className='err';st.textContent='Error: '+xhr.responseText;"
              "btn.disabled=false;"
            "}"
          "};"
          "xhr.onerror=function(){"
            "st.className='err';"
            "st.textContent='Upload failed — check device connection';"
            "btn.disabled=false;"
          "};"
          "xhr.send(f);"
        "}"
        "</script>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── POST /ota/upload ─────────────────────────────────────────────────────── */

#define OTA_BUF_SIZE 4096

static esp_err_t ota_upload_post(httpd_req_t *req)
{
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "OTA: no update partition found");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA: writing to '%s' at 0x%lx (size 0x%lx)",
             update_partition->label,
             (unsigned long)update_partition->address,
             (unsigned long)update_partition->size);

    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    char *buf = malloc(OTA_BUF_SIZE);
    if (!buf) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    int written   = 0;
    bool ok       = true;

    ESP_LOGI(TAG, "OTA: firmware size %d bytes", remaining);

    while (remaining > 0) {
        int to_recv  = (remaining < OTA_BUF_SIZE) ? remaining : OTA_BUF_SIZE;
        int received = httpd_req_recv(req, buf, to_recv);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "OTA: recv error %d", received);
            ok = false;
            break;
        }
        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA: write failed: %s", esp_err_to_name(err));
            ok = false;
            break;
        }
        remaining -= received;
        written   += received;
    }

    free(buf);

    if (!ok) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
        return ESP_FAIL;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            err == ESP_ERR_OTA_VALIDATE_FAILED
                                ? "Image validation failed — wrong binary?"
                                : "OTA end failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: set boot partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA: success — %d bytes written, rebooting", written);
    httpd_resp_sendstr(req, "OK");

    xTaskCreate(reboot_task, "ota_reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req,
        "<!DOCTYPE html><html><head><meta charset=UTF-8></head>"
        "<body style='font-family:sans-serif;padding:20px'>"
        "<h2>Rebooting...</h2></body></html>", HTTPD_RESP_USE_STRLEN);
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* ── Start server ─────────────────────────────────────────────────────────── */

esp_err_t config_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 16;
    cfg.stack_size       = 6144;   /* page builders use sizeable stack buffers */

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/",                .method = HTTP_GET,  .handler = root_get             },
        { .uri = "/control",         .method = HTTP_GET,  .handler = control_get          },
        { .uri = "/control/cmd",     .method = HTTP_POST, .handler = control_cmd_post     },
        { .uri = "/control/rc",      .method = HTTP_GET,  .handler = control_rc_get       },
        { .uri = "/config",          .method = HTTP_GET,  .handler = config_get           },
        { .uri = "/config/hostname", .method = HTTP_POST, .handler = config_hostname_post },
        { .uri = "/config/wifi",     .method = HTTP_POST, .handler = config_wifi_post     },
        { .uri = "/config/mqtt",     .method = HTTP_POST, .handler = config_mqtt_post     },
        { .uri = "/config/somfy",    .method = HTTP_POST, .handler = config_somfy_post    },
        { .uri = "/config/rolling",  .method = HTTP_POST, .handler = config_rolling_post  },
        { .uri = "/reboot",          .method = HTTP_POST, .handler = reboot_post          },
        { .uri = "/ota",             .method = HTTP_GET,  .handler = ota_get              },
        { .uri = "/ota/upload",      .method = HTTP_POST, .handler = ota_upload_post      },
    };
    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

    ESP_LOGI(TAG, "started on port 80");
    return ESP_OK;
}
