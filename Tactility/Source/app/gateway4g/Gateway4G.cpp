#include <app/event.h>
#include <app/manifest.h>
#include <app/scheduler.h>

#include <lvgl/lvgl.h>
#include <lvgl_window_manager/window_manager.h>
#include <lvgl/widgets/toolbar.h>

#include <tactility/check.h>
#include <tactility/memory.h>
#include <tactility/time.h>
#include <Tactility/Tactility.h>
#include <Tactility/Thread.h>
#include <Tactility/Timer.h>

#ifdef ESP_PLATFORM
#include <cJSON.h>
#include <esp_http_client.h>
#include <esp_mac.h>
#include <nvs.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace tt::app::gateway4g {

namespace {

constexpr auto* GATEWAY_URL = "http://10.1.10.12/health";

struct AutomationRuleInfo {
    int id = 0;
    std::string name;
    std::string trigger;
    int threshold = 0;
    std::string action;
    bool enabled = true;
    bool latched = false;
};

struct GeofenceZoneInfo {
    int idx = 0;
    std::string name;
    bool inside = false;
};

struct ScheduleEventInfo {
    uint32_t id = 0;
    std::string label;
    int64_t fireAt = 0;
    int repeat = 0; // 0=once, 1=daily, 2=weekly
    bool enabled = true;
};

struct GatewayData {
    bool online = false;
    bool atReady = false;
    bool pppUp = false;
    bool simReady = false;
    bool sdMounted = false;
    int csq = -1;
    int battery = -1;
    int uptime = 0;
    double temperature = 0;
    double humidity = 0;
    double pressure = 0;
    std::string firmware;
    std::string node;
    std::string operatorName;
    std::string netType;
    std::string netBand;
    std::string wanIp;
    bool gnssEnabled = false;
    bool gnssFix = false;
    double latitude = 0;
    double longitude = 0;
    int satellites = 0;
    bool callActive = false;
    bool callRinging = false;
    bool callHeld = false;
    std::string callNumber;
    int unreadSms = 0;
    // Contor lwIP GLOBAL de pachete (WiFi AP+STA+PPP combinate, nu doar
    // 4G) — gateway-ul nu are un contor de octeți per-interfață PPP fără
    // hook la nivel netif (evitat intenționat, zonă fragilă). Vezi
    // docs/PERFORMANCE.md din repo-ul gateway-ului.
    uint64_t txPackets = 0;
    uint64_t rxPackets = 0;
    std::string smsPreview;
    bool mqttConnected = false;
    std::string mqttUri;
    bool telegramConfigured = false;
    bool wolConfigured = false;
    std::string wolName;
    int automationRuleCount = 0;
    int automationLatchedCount = 0;
    int geofenceZoneCount = 0;
    int geofenceInsideCount = 0;
    std::vector<AutomationRuleInfo> automationRules;
    std::vector<GeofenceZoneInfo> geofenceZones;
    bool watchdogEnabled = false;
    bool watchdogOnline = false;
    std::string watchdogHost;
    int watchdogPort = 0;
    int watchdogFailures = 0;
    int watchdogThreshold = 0;
    std::vector<ScheduleEventInfo> scheduleEvents;
};

struct Context {
    uint32_t appInstanceId = 0;
    lv_obj_t* connection = nullptr;
    lv_obj_t* modem = nullptr;
    lv_obj_t* environment = nullptr;
    lv_obj_t* footer = nullptr;
    lv_obj_t* pairStatus = nullptr;
    lv_obj_t* pairCode = nullptr;
    lv_obj_t* phoneStatus = nullptr;
    lv_obj_t* phoneNumber = nullptr;
    lv_obj_t* smsNumber = nullptr;
    lv_obj_t* smsText = nullptr;
    lv_obj_t* smsInbox = nullptr;
    lv_obj_t* gnssStatus = nullptr;
    lv_obj_t* quickStatus = nullptr;
    // Tab "Config": stare motoare existente pe gateway (automation/telegram/
    // mqtt/geofence) + editare Wi-Fi/APN/MQTT + adaugare regula/zona, fara
    // sa fie nevoie de telefon langa gateway pt WebUI.
    lv_obj_t* configStatus = nullptr;
    lv_obj_t* configFeedback = nullptr;
    lv_obj_t* csqChart = nullptr;
    lv_chart_series_t* csqSeries = nullptr;
    lv_obj_t* staSsid = nullptr;
    lv_obj_t* staPass = nullptr;
    lv_obj_t* apn = nullptr;
    lv_obj_t* mqttUri = nullptr;
    lv_obj_t* mqttUser = nullptr;
    lv_obj_t* mqttPass = nullptr;
    lv_obj_t* telegramToken = nullptr;
    lv_obj_t* telegramChat = nullptr;
    lv_obj_t* ruleName = nullptr;
    lv_obj_t* ruleTrigger = nullptr;
    lv_obj_t* ruleThreshold = nullptr;
    lv_obj_t* ruleAction = nullptr;
    lv_obj_t* geoName = nullptr;
    lv_obj_t* geoRadius = nullptr;
    lv_obj_t* ruleList = nullptr;
    lv_obj_t* ruleDeleteId = nullptr;
    lv_obj_t* geoList = nullptr;
    lv_obj_t* geoDeleteIdx = nullptr;
    lv_obj_t* watchdogStatus = nullptr;
    lv_obj_t* watchdogHost = nullptr;
    lv_obj_t* watchdogPort = nullptr;
    lv_obj_t* watchdogInterval = nullptr;
    lv_obj_t* watchdogFailures = nullptr;
    lv_obj_t* watchdogSmsTo = nullptr;
    lv_obj_t* watchdogEnabledSwitch = nullptr;
    lv_obj_t* scheduleList = nullptr;
    lv_obj_t* scheduleLabel = nullptr;
    lv_obj_t* scheduleMinutes = nullptr;
    lv_obj_t* scheduleRepeat = nullptr;
    lv_obj_t* scheduleSmsTo = nullptr;
    lv_obj_t* scheduleSmsText = nullptr;
    lv_obj_t* scheduleDeleteId = nullptr;
    // Coada separata pt istoricul CSQ (nu concureaza cu refreshWorker/
    // commandWorker — refresh-ul principal ramane neblocat de asta).
    std::unique_ptr<Thread> csqWorker;
    std::string nodeId;
    std::string key;
    bool paired = false;
    std::mutex credLock;
    WindowId window = 0;
    std::unique_ptr<Timer> refreshTimer;
    std::unique_ptr<Timer> csqTimer;
    std::unique_ptr<Thread> refreshWorker;
    std::unique_ptr<Thread> commandWorker;
};

struct Creds {
    std::string nodeId;
    std::string key;
    bool paired;
};

Creds creds(Context* ctx) {
    std::lock_guard lock(ctx->credLock);
    return { ctx->nodeId, ctx->key, ctx->paired };
}

void setLabel(lv_obj_t* label, const char* text) {
    if (label != nullptr && lv_obj_is_valid(label)) {
        lv_label_set_text(label, text);
    }
}

// Network calls block for seconds, so they run on a worker thread instead of the LVGL or timer task.
// Returns false when the previous job on this worker is still running.
bool runAsync(std::unique_ptr<Thread>& worker, std::function<void()> job) {
    if (worker) {
        if (worker->getState() != Thread::State::Stopped) return false;
        worker->join();
        worker.reset();
    }
    worker = std::make_unique<Thread>("gw4g", 6144, [job = std::move(job)] {
        job();
        return 0;
    });
    worker->start();
    return true;
}

void stopWorker(std::unique_ptr<Thread>& worker) {
    if (worker) {
        worker->join();
        worker.reset();
    }
}

// Must be called from a worker thread: takes the LVGL lock and skips widgets of a buried window.
void postLabel(Context* ctx, lv_obj_t* label, const std::string& text) {
    lvgl_lock();
    if (window_manager_get_state(ctx->window) == WINDOW_STATE_GRANTED) {
        setLabel(label, text.c_str());
    }
    lvgl_unlock();
}

#ifdef ESP_PLATFORM
bool jsonBool(cJSON* root, const char* key, bool fallback = false) {
    auto* item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}

int jsonInt(cJSON* root, const char* key, int fallback = 0) {
    auto* item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

double jsonDouble(cJSON* root, const char* key, double fallback = 0) {
    auto* item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

std::string jsonString(cJSON* root, const char* key) {
    auto* item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

esp_err_t httpEvent(esp_http_client_event_t* event) {
    if (event->event_id == HTTP_EVENT_ON_DATA && event->user_data && event->data_len > 0) {
        static_cast<std::string*>(event->user_data)->append(static_cast<const char*>(event->data), event->data_len);
    }
    return ESP_OK;
}

bool request(const char* url, esp_http_client_method_t method, const char* body,
             const std::string& nodeId, const std::string& key,
             std::string& response, int timeout = 5000) {
    esp_http_client_config_t config {};
    config.url = url;
    config.method = method;
    config.timeout_ms = timeout;
    config.buffer_size = 2048;
    config.event_handler = httpEvent;
    config.user_data = &response;

    auto* client = esp_http_client_init(&config);
    if (client == nullptr) return false;
    if (!nodeId.empty() && !key.empty()) {
        esp_http_client_set_header(client, "X-DeskOS-Node", nodeId.c_str());
        esp_http_client_set_header(client, "X-DeskOS-Key", key.c_str());
    }
    if (body != nullptr) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, std::strlen(body));
    }
    const auto result = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return result == ESP_OK && status >= 200 && status < 300;
}

void initIdentity(Context* ctx) {
    uint8_t mac[6] {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char id[32];
    // DeskOS node IDs are exactly 12 characters (DESKOS_NODE_ID_LEN - 1).
    std::snprintf(id, sizeof(id), "THMI%02X%02X%02X%02X", mac[2], mac[3], mac[4], mac[5]);
    ctx->nodeId = id;
}

bool loadCredentials(Context* ctx) {
    nvs_handle_t handle;
    if (nvs_open("gateway4g", NVS_READONLY, &handle) != ESP_OK) return false;
    char key[65] {};
    size_t length = sizeof(key);
    const auto result = nvs_get_str(handle, "key", key, &length);
    nvs_close(handle);
    if (result != ESP_OK || std::strlen(key) != 64) return false;
    ctx->key = key;
    ctx->paired = true;
    return true;
}

void saveCredentials(const std::string& key) {
    nvs_handle_t handle;
    if (nvs_open("gateway4g", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_str(handle, "key", key.c_str());
        nvs_commit(handle);
        nvs_close(handle);
    }
}

bool pairGateway(Context* ctx, const std::string& pairCode) {
    if (pairCode.size() != 6) return false;

    char claim[320];
    std::snprintf(claim, sizeof(claim),
        "{\"code\":\"%s\",\"id\":\"%s\",\"name\":\"Tactility T-HMI\",\"role\":\"console\",\"capabilities\":\"gateway,phone,sms,gnss\"}",
        pairCode.c_str(), ctx->nodeId.c_str());
    std::string claimResponse;
    if (!request("http://10.1.10.12/api/pairing/claim", HTTP_METHOD_POST, claim, {}, {}, claimResponse)) return false;
    auto* reply = cJSON_Parse(claimResponse.c_str());
    auto* key = reply ? cJSON_GetObjectItemCaseSensitive(reply, "key") : nullptr;
    std::string newKey = cJSON_IsString(key) && key->valuestring ? key->valuestring : "";
    cJSON_Delete(reply);
    if (newKey.size() != 64) return false;
    saveCredentials(newKey);
    std::lock_guard lock(ctx->credLock);
    ctx->key = newKey;
    ctx->paired = true;
    return true;
}

bool postCommandJson(Context* ctx, cJSON* json) {
    const auto identity = creds(ctx);
    if (!identity.paired) { cJSON_Delete(json); return false; }
    char* encoded = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!encoded) return false;
    std::string response;
    const bool ok = request("http://10.1.10.12/api/node/control", HTTP_METHOD_POST,
        encoded, identity.nodeId, identity.key, response, 30000);
    cJSON_free(encoded);
    return ok;
}

bool sendCommand(Context* ctx, const char* command, const char* value = nullptr, const char* text = nullptr) {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "command", command);
    if (value) cJSON_AddStringToObject(json, "value", value);
    if (text) cJSON_AddStringToObject(json, "text", text);
    return postCommandJson(ctx, json);
}

// Comenzi cu mai multe campuri decat perechea generica value/text suporta —
// vezi node_gateway_control_post() in webserver.c (gateway) pt formatul
// exact asteptat de fiecare.
bool sendMqttConfig(Context* ctx, const std::string& uri, const std::string& user, const std::string& pass) {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "command", "set_mqtt");
    cJSON_AddStringToObject(json, "value", uri.c_str());
    if (!user.empty()) cJSON_AddStringToObject(json, "text", user.c_str());
    if (!pass.empty()) cJSON_AddStringToObject(json, "mqtt_pass", pass.c_str());
    return postCommandJson(ctx, json);
}

bool sendAutomationRule(Context* ctx, const std::string& name, const std::string& trigger,
                         int threshold, const std::string& action) {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "command", "automation_upsert");
    cJSON* rule = cJSON_AddObjectToObject(json, "rule");
    cJSON_AddStringToObject(rule, "name", name.c_str());
    cJSON_AddStringToObject(rule, "trigger", trigger.c_str());
    cJSON_AddNumberToObject(rule, "threshold", threshold);
    cJSON_AddStringToObject(rule, "action", action.c_str());
    cJSON_AddBoolToObject(rule, "enabled", true);
    return postCommandJson(ctx, json);
}

bool sendGeofenceZone(Context* ctx, const std::string& name, double lat, double lon, double radius) {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "command", "geofence_add");
    cJSON_AddStringToObject(json, "value", name.c_str());
    cJSON_AddNumberToObject(json, "lat", lat);
    cJSON_AddNumberToObject(json, "lon", lon);
    cJSON_AddNumberToObject(json, "radius", radius);
    return postCommandJson(ctx, json);
}

// automation_delete/geofence_remove cer "value" ca NUMAR JSON (nu string —
// vezi node_gateway_control_post() in webserver.c), spre deosebire de restul
// comenzilor simple; de-asta nu folosim command()/sendCommand() aici (acelea
// trimit mereu string).
// int64_t, nu int: ID-urile de scheduler vin din esp_random() (0..UINT32_MAX
// complet), care poate depasi INT32_MAX — un `int` ar trunchia/UB la parsare.
bool sendDeleteById(Context* ctx, const char* deleteCommand, int64_t id) {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "command", deleteCommand);
    cJSON_AddNumberToObject(json, "value", static_cast<double>(id));
    return postCommandJson(ctx, json);
}

bool sendWatchdogConfig(Context* ctx, bool enabled, const std::string& host, int port,
                        int intervalS, int failures, const std::string& smsTo) {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "command", "host_watchdog_set");
    cJSON_AddBoolToObject(json, "enabled", enabled);
    cJSON_AddStringToObject(json, "host", host.c_str());
    cJSON_AddNumberToObject(json, "port", port);
    cJSON_AddNumberToObject(json, "interval_s", intervalS);
    cJSON_AddNumberToObject(json, "failures", failures);
    cJSON_AddStringToObject(json, "sms_to", smsTo.c_str());
    cJSON_AddBoolToObject(json, "notify_recovery", true);
    return postCommandJson(ctx, json);
}

// fireAt = timp Unix absolut (calculat pe T-HMI din "peste N minute" +
// ceasul local, sincronizat NTP) — gateway-ul nu stie "acum" din perspectiva
// T-HMI, doar accepta un timestamp absolut (vezi scheduler_upsert_json in
// scheduler.c, care cere fire_at >= VALID_TIME_EPOCH).
bool sendScheduleEvent(Context* ctx, const std::string& label, int64_t fireAt, int repeat,
                        const std::string& smsTo, const std::string& smsText) {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "command", "schedule_upsert");
    cJSON* event = cJSON_AddObjectToObject(json, "event");
    cJSON_AddStringToObject(event, "label", label.c_str());
    cJSON_AddNumberToObject(event, "fire_at", static_cast<double>(fireAt));
    cJSON_AddNumberToObject(event, "repeat", repeat);
    cJSON_AddStringToObject(event, "sms_to", smsTo.c_str());
    cJSON_AddStringToObject(event, "sms_text", smsText.c_str());
    cJSON_AddBoolToObject(event, "enabled", true);
    return postCommandJson(ctx, json);
}

bool fetchGateway(Context* ctx, GatewayData& data) {
    std::string body;
    const auto identity = creds(ctx);
    const char* url = identity.paired ? "http://10.1.10.12/api/node/gateway" : GATEWAY_URL;
    if (!request(url, HTTP_METHOD_GET, nullptr, identity.nodeId, identity.key, body)) return false;

    auto* root = cJSON_Parse(body.c_str());
    if (root == nullptr) return false;

    data.online = true;
    data.atReady = jsonBool(root, "at_ready");
    data.pppUp = jsonBool(root, "ppp_up");
    data.simReady = jsonBool(root, "sim_ready", data.atReady);
    data.sdMounted = jsonBool(root, "sd_mounted");
    data.csq = jsonInt(root, "csq", -1);
    data.battery = jsonInt(root, "battery_percent", -1);
    data.uptime = jsonInt(root, "uptime_s");
    data.temperature = jsonDouble(root, "bmp_temp_c");
    data.humidity = jsonDouble(root, "bmp_humidity");
    data.pressure = jsonDouble(root, "bmp_pressure_hpa");
    data.firmware = jsonString(root, "fw");
    data.node = jsonString(root, "node");
    data.operatorName = jsonString(root, "operator");
    data.netType = jsonString(root, "net_type");
    data.netBand = jsonString(root, "net_band");
    data.wanIp = jsonString(root, "wan_ip");
    data.gnssEnabled = jsonBool(root, "gnss_enabled");
    data.gnssFix = jsonBool(root, "gnss_fix");
    data.latitude = jsonDouble(root, "latitude");
    data.longitude = jsonDouble(root, "longitude");
    data.satellites = jsonInt(root, "satellites");
    data.callActive = jsonBool(root, "call_active");
    data.callRinging = jsonBool(root, "call_ringing");
    data.callHeld = jsonBool(root, "call_held");
    data.callNumber = jsonString(root, "call_number");
    data.unreadSms = jsonInt(root, "unread_sms");
    data.txPackets = static_cast<uint64_t>(jsonDouble(root, "tx_packets"));
    data.rxPackets = static_cast<uint64_t>(jsonDouble(root, "rx_packets"));
    data.mqttConnected = jsonBool(root, "mqtt_connected");
    data.mqttUri = jsonString(root, "mqtt_uri");
    data.telegramConfigured = jsonBool(root, "telegram_configured");
    data.wolConfigured = jsonBool(root, "wol_configured");
    data.wolName = jsonString(root, "wol_name");
    auto* rules = cJSON_GetObjectItemCaseSensitive(root, "automation_rules");
    if (cJSON_IsArray(rules)) {
        data.automationRuleCount = cJSON_GetArraySize(rules);
        cJSON* rule = nullptr;
        cJSON_ArrayForEach(rule, rules) {
            if (jsonBool(rule, "latched")) data.automationLatchedCount++;
            AutomationRuleInfo info;
            info.id = jsonInt(rule, "id");
            info.name = jsonString(rule, "name");
            info.trigger = jsonString(rule, "trigger");
            info.threshold = jsonInt(rule, "threshold");
            info.action = jsonString(rule, "action");
            info.enabled = jsonBool(rule, "enabled", true);
            info.latched = jsonBool(rule, "latched");
            data.automationRules.push_back(std::move(info));
        }
    }
    auto* zones = cJSON_GetObjectItemCaseSensitive(root, "geofence_zones");
    if (cJSON_IsArray(zones)) {
        data.geofenceZoneCount = cJSON_GetArraySize(zones);
        cJSON* zone = nullptr;
        cJSON_ArrayForEach(zone, zones) {
            if (jsonBool(zone, "inside")) data.geofenceInsideCount++;
            GeofenceZoneInfo info;
            info.idx = jsonInt(zone, "idx");
            info.name = jsonString(zone, "name");
            info.inside = jsonBool(zone, "inside");
            data.geofenceZones.push_back(std::move(info));
        }
    }
    data.watchdogEnabled = jsonBool(root, "watchdog_enabled");
    data.watchdogOnline = jsonBool(root, "watchdog_online");
    data.watchdogHost = jsonString(root, "watchdog_host");
    data.watchdogPort = jsonInt(root, "watchdog_port");
    data.watchdogFailures = jsonInt(root, "watchdog_failures");
    data.watchdogThreshold = jsonInt(root, "watchdog_threshold");
    auto* scheduleEvents = cJSON_GetObjectItemCaseSensitive(root, "schedule_events");
    if (cJSON_IsArray(scheduleEvents)) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, scheduleEvents) {
            ScheduleEventInfo info;
            info.id = static_cast<uint32_t>(jsonDouble(item, "id"));
            info.label = jsonString(item, "label");
            info.fireAt = static_cast<int64_t>(jsonDouble(item, "fire_at"));
            info.repeat = jsonInt(item, "repeat");
            info.enabled = jsonBool(item, "enabled", true);
            data.scheduleEvents.push_back(std::move(info));
        }
    }
    auto* sms = cJSON_GetObjectItemCaseSensitive(root, "sms");
    if (cJSON_IsArray(sms) && cJSON_GetArraySize(sms) > 0) {
        auto* first = cJSON_GetArrayItem(sms, 0);
        data.smsPreview = jsonString(first, "number") + ": " + jsonString(first, "text");
    }
    cJSON_Delete(root);
    return true;
}

// Ultimele (cel mult) 60 valori CSQ, cele mai vechi primele — gata pt un
// lv_chart cu point_count fix. Gol daca cererea eșuează sau nu e pereche.
std::vector<int> fetchCsqHistory(Context* ctx) {
    std::vector<int> out;
    const auto identity = creds(ctx);
    if (!identity.paired) return out;
    std::string body;
    if (!request("http://10.1.10.12/api/node/csq_history", HTTP_METHOD_GET, nullptr,
                 identity.nodeId, identity.key, body)) return out;
    auto* root = cJSON_Parse(body.c_str());
    if (!cJSON_IsArray(root)) { cJSON_Delete(root); return out; }
    int count = cJSON_GetArraySize(root);
    int start = count > 60 ? count - 60 : 0;
    for (int i = start; i < count; ++i) {
        out.push_back(jsonInt(cJSON_GetArrayItem(root, i), "csq", -1));
    }
    cJSON_Delete(root);
    return out;
}
#else
void initIdentity(Context*) {}
bool loadCredentials(Context*) { return false; }
bool pairGateway(Context*, const std::string&) { return false; }
bool sendCommand(Context*, const char*, const char* = nullptr, const char* = nullptr) { return false; }
bool sendMqttConfig(Context*, const std::string&, const std::string&, const std::string&) { return false; }
bool sendAutomationRule(Context*, const std::string&, const std::string&, int, const std::string&) { return false; }
bool sendGeofenceZone(Context*, const std::string&, double, double, double) { return false; }
bool sendDeleteById(Context*, const char*, int64_t) { return false; }
bool sendWatchdogConfig(Context*, bool, const std::string&, int, int, int, const std::string&) { return false; }
bool sendScheduleEvent(Context*, const std::string&, int64_t, int, const std::string&, const std::string&) { return false; }
bool fetchGateway(Context*, GatewayData&) { return false; }
std::vector<int> fetchCsqHistory(Context*) { return {}; }
#endif

void render(Context* ctx, const GatewayData& data) {
    char line[192];
    if (!data.online) {
        setLabel(ctx->connection, "OFFLINE  10.1.10.12");
        setLabel(ctx->modem, "Gateway indisponibil\nVerifica Wi-Fi-ul T-HMI");
        setLabel(ctx->environment, "Fara date");
        setLabel(ctx->footer, "Reincerc automat in 5 secunde");
        return;
    }

    std::snprintf(line, sizeof(line), "ONLINE  %s", data.node.empty() ? "10.1.10.12" : data.node.c_str());
    setLabel(ctx->connection, line);

    std::snprintf(line, sizeof(line), "%s  %s %s\nSemnal %d/31   Date %s\nWAN %s",
        data.operatorName.empty() ? "Operator --" : data.operatorName.c_str(),
        data.netType.empty() ? "4G" : data.netType.c_str(), data.netBand.c_str(),
        data.csq, data.pppUp ? "ACTIVE" : "OPRITE",
        data.wanIp.empty() ? "--" : data.wanIp.c_str());
    setLabel(ctx->modem, line);

    if (data.txPackets || data.rxPackets) {
        // Pachete lwIP totale pe placa gateway (WiFi AP+STA+PPP combinate),
        // nu octeți — gateway-ul nu are un contor de octeți per-interfață 4G.
        std::snprintf(line, sizeof(line), "RX %llu   TX %llu pachete (gateway, total)",
            static_cast<unsigned long long>(data.rxPackets),
            static_cast<unsigned long long>(data.txPackets));
    } else {
        std::snprintf(line, sizeof(line), "Trafic: fara date inca");
    }
    setLabel(ctx->quickStatus, line);

    std::snprintf(line, sizeof(line), "%.1f C   %.0f %%RH\n%.1f hPa   SD %s",
        data.temperature, data.humidity, data.pressure, data.sdMounted ? "OK" : "--");
    setLabel(ctx->environment, line);

    if (ctx->paired) {
        std::snprintf(line, sizeof(line), "PAIR OK  %s / %s %s\nWAN %s",
            data.operatorName.empty() ? "operator --" : data.operatorName.c_str(),
            data.netType.empty() ? "--" : data.netType.c_str(), data.netBand.c_str(),
            data.wanIp.empty() ? "--" : data.wanIp.c_str());
        setLabel(ctx->pairStatus, line);
        std::snprintf(line, sizeof(line), "%s%s  %s",
            data.callRinging ? "RINGING " : (data.callActive ? "ACTIVE " : "IDLE "),
            data.callHeld ? "(HOLD)" : "", data.callNumber.c_str());
        setLabel(ctx->phoneStatus, line);
        std::snprintf(line, sizeof(line), "GNSS %s | Fix %s | Sat %d\n%.6f, %.6f",
            data.gnssEnabled ? "ON" : "OFF", data.gnssFix ? "YES" : "NO",
            data.satellites, data.latitude, data.longitude);
        setLabel(ctx->gnssStatus, line);
        std::snprintf(line, sizeof(line), "Unread: %d\n%s", data.unreadSms,
            data.smsPreview.empty() ? "No messages" : data.smsPreview.c_str());
        setLabel(ctx->smsInbox, line);
    }

    std::snprintf(line, sizeof(line), "%s  |  uptime %dh %02dm  |  refresh 5s",
        data.firmware.empty() ? "gateway" : data.firmware.c_str(),
        data.uptime / 3600, (data.uptime / 60) % 60);
    setLabel(ctx->footer, line);

    if (ctx->paired && ctx->configStatus != nullptr) {
        // Notificare vizuala: orice regula automation "latched" acum sau
        // baterie critica apare cu ATENTIE in fata — restul motoarelor (deja
        // functionale pe gateway, doar neexpuse pana acum) arata starea lor.
        const bool lowBattery = data.battery >= 0 && data.battery < 15;
        char alert[64] = "";
        if (data.automationLatchedCount > 0) {
            std::snprintf(alert, sizeof(alert), "ATENTIE: %d regula(i) activa(e)\n", data.automationLatchedCount);
        } else if (lowBattery) {
            std::snprintf(alert, sizeof(alert), "ATENTIE: baterie gateway %d%%\n", data.battery);
        }
        std::snprintf(line, sizeof(line),
            "%sMQTT %s   Telegram %s\nAutomation: %d reguli%s   Geofence: %d zone%s\nWOL: %s",
            alert,
            data.mqttConnected ? "conectat" : "oprit",
            data.telegramConfigured ? "configurat" : "neconfigurat",
            data.automationRuleCount,
            data.automationLatchedCount > 0 ? " (activa!)" : "",
            data.geofenceZoneCount,
            data.geofenceInsideCount > 0 ? " (in zona)" : "",
            data.wolConfigured ? data.wolName.empty() ? "configurat" : data.wolName.c_str() : "neconfigurat");
        setLabel(ctx->configStatus, line);

        if (ctx->ruleList != nullptr) {
            std::string text;
            for (const auto& rule : data.automationRules) {
                char row[128];
                std::snprintf(row, sizeof(row), "#%d %s: %s%s%s -> %s%s\n",
                    rule.id, rule.name.c_str(), rule.trigger.c_str(),
                    rule.trigger == "internet_down" ? "" : " < ",
                    rule.trigger == "internet_down" ? "" : std::to_string(rule.threshold).c_str(),
                    rule.action.c_str(), rule.latched ? " (ACTIVA)" : "");
                text += row;
            }
            setLabel(ctx->ruleList, text.empty() ? "Nicio regula inca." : text.c_str());
        }
        if (ctx->geoList != nullptr) {
            std::string text;
            for (const auto& zone : data.geofenceZones) {
                char row[96];
                std::snprintf(row, sizeof(row), "#%d %s%s\n", zone.idx, zone.name.c_str(),
                    zone.inside ? " (esti aici)" : "");
                text += row;
            }
            setLabel(ctx->geoList, text.empty() ? "Nicio zona inca." : text.c_str());
        }
        if (ctx->watchdogStatus != nullptr) {
            char wdLine[128];
            if (!data.watchdogEnabled) {
                std::snprintf(wdLine, sizeof(wdLine), "Oprit. Configureaza mai jos ca sa-l activezi.");
            } else {
                std::snprintf(wdLine, sizeof(wdLine), "%s:%d — %s (%d/%d esecuri)",
                    data.watchdogHost.empty() ? "--" : data.watchdogHost.c_str(), data.watchdogPort,
                    data.watchdogOnline ? "ONLINE" : "DOWN", data.watchdogFailures, data.watchdogThreshold);
            }
            setLabel(ctx->watchdogStatus, wdLine);
        }
        if (ctx->scheduleList != nullptr) {
            std::string text;
            for (const auto& event : data.scheduleEvents) {
                std::time_t fireTime = static_cast<std::time_t>(event.fireAt);
                std::tm local {};
                localtime_r(&fireTime, &local);
                char stamp[24];
                std::strftime(stamp, sizeof(stamp), "%d.%m %H:%M", &local);
                static const char* repeatText[] = {"o data", "zilnic", "saptamanal"};
                char row[160];
                std::snprintf(row, sizeof(row), "#%u %s — %s (%s)%s\n", static_cast<unsigned int>(event.id), event.label.c_str(),
                    stamp, repeatText[event.repeat < 3 ? event.repeat : 0], event.enabled ? "" : " [oprit]");
                text += row;
            }
            setLabel(ctx->scheduleList, text.empty() ? "Niciun eveniment inca." : text.c_str());
        }
    }
}

void refresh(Context* ctx) {
    GatewayData data;
    fetchGateway(ctx, data);
    lvgl_lock();
    if (window_manager_get_state(ctx->window) == WINDOW_STATE_GRANTED) {
        render(ctx, data);
    }
    lvgl_unlock();
}

// Runs from the timer task: only queues the request, never blocks.
void refreshAsync(Context* ctx) {
    runAsync(ctx->refreshWorker, [ctx] { refresh(ctx); });
}

void refreshCsq(Context* ctx) {
    const auto history = fetchCsqHistory(ctx);
    if (history.empty()) return;
    lvgl_lock();
    if (window_manager_get_state(ctx->window) == WINDOW_STATE_GRANTED &&
        ctx->csqChart != nullptr && ctx->csqSeries != nullptr) {
        lv_chart_set_point_count(ctx->csqChart, static_cast<uint32_t>(history.size()));
        std::vector<int32_t> values(history.begin(), history.end());
        lv_chart_set_series_values(ctx->csqChart, ctx->csqSeries, values.data(), values.size());
        lv_chart_refresh(ctx->csqChart);
    }
    lvgl_unlock();
}

// Rulează pe propria coadă (csqWorker), separată de refreshWorker — istoricul
// e o cerere HTTP separată, mai rar necesară, n-are voie să întârzie
// refresh-ul principal de stare (la 5s).
void refreshCsqAsync(Context* ctx) {
    runAsync(ctx->csqWorker, [ctx] { refreshCsq(ctx); });
}

// Called from LVGL event handlers: copies the inputs, then sends the command off the LVGL thread.
void command(Context* ctx, lv_obj_t* target, const char* name, const char* okText, const char* failText,
             lv_obj_t* valueField = nullptr, lv_obj_t* textField = nullptr) {
    const std::string value = valueField ? lv_textarea_get_text(valueField) : "";
    const std::string text = textField ? lv_textarea_get_text(textField) : "";
    const bool started = runAsync(ctx->commandWorker, [=] {
        const bool ok = sendCommand(ctx, name, valueField ? value.c_str() : nullptr, textField ? text.c_str() : nullptr);
        postLabel(ctx, target, ok ? okText : failText);
    });
    setLabel(target, started ? "Se trimite..." : "Ocupat, incearca din nou");
}

lv_obj_t* createCard(lv_obj_t* parent, const char* title, lv_obj_t** value) {
    auto* card = lv_obj_create(parent);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    auto* heading = lv_label_create(card);
    lv_label_set_text(heading, title);
    lv_obj_set_style_text_color(heading, lv_theme_get_color_primary(heading), 0);
    *value = lv_label_create(card);
    lv_label_set_text(*value, "Se conecteaza...");
    return card;
}

void onBackPressed(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    app_event_emit_close(ctx->appInstanceId);
}

lv_obj_t* addButton(lv_obj_t* parent, const char* text, lv_event_cb_t callback, Context* ctx) {
    auto* button = lv_button_create(parent);
    lv_obj_set_height(button, 38);
    lv_obj_add_event_cb(button, callback, LV_EVENT_SHORT_CLICKED, ctx);
    auto* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}

void onPair(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    const std::string code = lv_textarea_get_text(ctx->pairCode);
    const bool started = runAsync(ctx->commandWorker, [ctx, code] {
        const bool ok = pairGateway(ctx, code);
        postLabel(ctx, ctx->pairStatus, ok ? "PAIR OK - secure link active" : "Pairing failed - retry");
    });
    setLabel(ctx->pairStatus, started ? "Pairing..." : "Ocupat, incearca din nou");
}

void onDial(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    command(ctx, ctx->phoneStatus, "dial", "Dial command sent", "Dial failed", ctx->phoneNumber);
}

void onAnswer(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    command(ctx, ctx->phoneStatus, "answer", "Answered", "Answer failed");
}

void onHold(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    command(ctx, ctx->phoneStatus, "hold", "Hold toggled", "Hold failed");
}

void onHangup(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    command(ctx, ctx->phoneStatus, "hangup", "Call ended", "Hangup failed");
}

void onSendSms(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    command(ctx, ctx->smsInbox, "sms", "SMS sent", "SMS failed", ctx->smsNumber, ctx->smsText);
}

void onReadSms(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    command(ctx, ctx->smsInbox, "sms_read_all", "Inbox marked read", "Command failed");
}

void onGnssOn(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    command(ctx, ctx->gnssStatus, "gnss_on", "GNSS enabling...", "GNSS command failed");
}

void onGnssOff(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    command(ctx, ctx->gnssStatus, "gnss_off", "GNSS disabling...", "GNSS command failed");
}

void onModemRestart(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    command(ctx, ctx->quickStatus, "modem_reconnect",
        "Restart modem trimis; reconectare in curs...", "Restart modem esuat");
}

void onDataOn(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    command(ctx, ctx->quickStatus, "data_on", "Date mobile activate", "Gateway-ul necesita update pentru DATA ON");
}

void onDataOff(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    command(ctx, ctx->quickStatus, "data_off", "Date mobile oprite", "Gateway-ul necesita update pentru DATA OFF");
}

void onSaveWifi(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    command(ctx, ctx->configFeedback, "set_sta", "Wi-Fi salvat pe gateway", "Wi-Fi esuat (SSID lipsa?)",
        ctx->staSsid, ctx->staPass);
}

void onSaveApn(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    command(ctx, ctx->configFeedback, "set_apn", "APN salvat pe gateway", "APN esuat (camp gol?)", ctx->apn);
}

void onSaveMqtt(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    const std::string uri = lv_textarea_get_text(ctx->mqttUri);
    const std::string user = lv_textarea_get_text(ctx->mqttUser);
    const std::string pass = lv_textarea_get_text(ctx->mqttPass);
    const bool started = runAsync(ctx->commandWorker, [ctx, uri, user, pass] {
        const bool ok = sendMqttConfig(ctx, uri, user, pass);
        postLabel(ctx, ctx->configFeedback, ok ? "MQTT salvat pe gateway" : "MQTT esuat (URI lipsa?)");
    });
    setLabel(ctx->configFeedback, started ? "Se trimite..." : "Ocupat, incearca din nou");
}

void onSaveTelegram(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    command(ctx, ctx->configFeedback, "telegram_set", "Telegram salvat pe gateway", "Telegram esuat",
        ctx->telegramToken, ctx->telegramChat);
}

constexpr const char* AUTOMATION_TRIGGERS[] = { "internet_down", "csq_below", "battery_below" };
constexpr const char* AUTOMATION_ACTIONS[] = { "alert", "broadcast", "led" };

void onAddRule(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    const std::string name = lv_textarea_get_text(ctx->ruleName);
    const std::string thresholdText = lv_textarea_get_text(ctx->ruleThreshold);
    const int threshold = thresholdText.empty() ? 0 : std::atoi(thresholdText.c_str());
    const uint32_t triggerIdx = lv_dropdown_get_selected(ctx->ruleTrigger);
    const uint32_t actionIdx = lv_dropdown_get_selected(ctx->ruleAction);
    const std::string trigger = AUTOMATION_TRIGGERS[triggerIdx < 3 ? triggerIdx : 0];
    const std::string action = AUTOMATION_ACTIONS[actionIdx < 3 ? actionIdx : 0];
    if (name.empty()) {
        setLabel(ctx->configFeedback, "Regula are nevoie de un nume");
        return;
    }
    const bool started = runAsync(ctx->commandWorker, [ctx, name, trigger, threshold, action] {
        const bool ok = sendAutomationRule(ctx, name, trigger, threshold, action);
        postLabel(ctx, ctx->configFeedback, ok ? "Regula automation salvata" : "Regula esuata");
    });
    setLabel(ctx->configFeedback, started ? "Se trimite..." : "Ocupat, incearca din nou");
}

void onAddGeofence(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    const std::string name = lv_textarea_get_text(ctx->geoName);
    const std::string radiusText = lv_textarea_get_text(ctx->geoRadius);
    const double radius = radiusText.empty() ? 0 : std::atof(radiusText.c_str());
    // Folosim pozitia GNSS curenta a gateway-ului (interogata proaspat mai
    // jos) — evita introducerea manuala de coordonate zecimale pe tastatura T-HMI.
    if (name.empty() || radius <= 0) {
        setLabel(ctx->configFeedback, "Zona are nevoie de nume si raza > 0");
        return;
    }
    const bool started = runAsync(ctx->commandWorker, [ctx, name, radius] {
        GatewayData data;
        fetchGateway(ctx, data);
        bool ok = false;
        if (data.gnssFix) {
            ok = sendGeofenceZone(ctx, name, data.latitude, data.longitude, radius);
        }
        postLabel(ctx, ctx->configFeedback,
            !data.gnssFix ? "Fara fix GNSS — nu stiu poziția curenta" :
            ok ? "Zona geofence salvata (poziția curenta)" : "Zona esuata");
    });
    setLabel(ctx->configFeedback, started ? "Se trimite..." : "Ocupat, incearca din nou");
}

void onDeleteRule(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    const std::string idText = lv_textarea_get_text(ctx->ruleDeleteId);
    if (idText.empty()) {
        setLabel(ctx->configFeedback, "Scrie ID-ul regulii (vezi #N din lista)");
        return;
    }
    const int id = std::atoi(idText.c_str());
    const bool started = runAsync(ctx->commandWorker, [ctx, id] {
        const bool ok = sendDeleteById(ctx, "automation_delete", id);
        postLabel(ctx, ctx->configFeedback, ok ? "Regula stearsa" : "Stergere esuata (ID gresit?)");
    });
    setLabel(ctx->configFeedback, started ? "Se trimite..." : "Ocupat, incearca din nou");
}

void onDeleteGeofence(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    const std::string idxText = lv_textarea_get_text(ctx->geoDeleteIdx);
    if (idxText.empty()) {
        setLabel(ctx->configFeedback, "Scrie indexul zonei (vezi #N din lista)");
        return;
    }
    const int idx = std::atoi(idxText.c_str());
    const bool started = runAsync(ctx->commandWorker, [ctx, idx] {
        const bool ok = sendDeleteById(ctx, "geofence_remove", idx);
        postLabel(ctx, ctx->configFeedback, ok ? "Zona stearsa" : "Stergere esuata (index gresit?)");
    });
    setLabel(ctx->configFeedback, started ? "Se trimite..." : "Ocupat, incearca din nou");
}

void onSaveWatchdog(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    const std::string host = lv_textarea_get_text(ctx->watchdogHost);
    const std::string portText = lv_textarea_get_text(ctx->watchdogPort);
    const std::string intervalText = lv_textarea_get_text(ctx->watchdogInterval);
    const std::string failuresText = lv_textarea_get_text(ctx->watchdogFailures);
    const std::string smsTo = lv_textarea_get_text(ctx->watchdogSmsTo);
    const bool enabled = lv_obj_has_state(ctx->watchdogEnabledSwitch, LV_STATE_CHECKED);
    if (host.empty() || smsTo.empty()) {
        setLabel(ctx->configFeedback, "Watchdog: completeaza host si numar SMS");
        return;
    }
    const int port = portText.empty() ? 443 : std::atoi(portText.c_str());
    const int interval = intervalText.empty() ? 60 : std::atoi(intervalText.c_str());
    const int failures = failuresText.empty() ? 3 : std::atoi(failuresText.c_str());
    const bool started = runAsync(ctx->commandWorker, [ctx, enabled, host, port, interval, failures, smsTo] {
        const bool ok = sendWatchdogConfig(ctx, enabled, host, port, interval, failures, smsTo);
        postLabel(ctx, ctx->configFeedback, ok ? "Watchdog salvat" : "Watchdog esuat");
    });
    setLabel(ctx->configFeedback, started ? "Se trimite..." : "Ocupat, incearca din nou");
}

void onAddScheduleEvent(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    const std::string label = lv_textarea_get_text(ctx->scheduleLabel);
    const std::string minutesText = lv_textarea_get_text(ctx->scheduleMinutes);
    const std::string smsTo = lv_textarea_get_text(ctx->scheduleSmsTo);
    const std::string smsText = lv_textarea_get_text(ctx->scheduleSmsText);
    if (label.empty() || minutesText.empty() || smsTo.empty() || smsText.empty()) {
        setLabel(ctx->configFeedback, "Completeaza toate campurile evenimentului");
        return;
    }
    const int minutes = std::atoi(minutesText.c_str());
    if (minutes <= 0) {
        setLabel(ctx->configFeedback, "Minutele trebuie sa fie un numar pozitiv");
        return;
    }
    const uint32_t repeatIdx = lv_dropdown_get_selected(ctx->scheduleRepeat);
    // Ceasul T-HMI trebuie sincronizat (NTP) — altfel fire_at calculat aici e greșit.
    const int64_t fireAt = static_cast<int64_t>(std::time(nullptr)) + static_cast<int64_t>(minutes) * 60;
    const bool started = runAsync(ctx->commandWorker, [ctx, label, fireAt, repeatIdx, smsTo, smsText] {
        const bool ok = sendScheduleEvent(ctx, label, fireAt, static_cast<int>(repeatIdx), smsTo, smsText);
        postLabel(ctx, ctx->configFeedback, ok ? "Eveniment salvat" : "Eveniment esuat");
    });
    setLabel(ctx->configFeedback, started ? "Se trimite..." : "Ocupat, incearca din nou");
}

void onDeleteScheduleEvent(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    const std::string idText = lv_textarea_get_text(ctx->scheduleDeleteId);
    if (idText.empty()) {
        setLabel(ctx->configFeedback, "Scrie ID-ul evenimentului (vezi # din lista)");
        return;
    }
    // strtoul, nu atoi: ID-urile de scheduler vin din esp_random() si pot
    // depasi INT32_MAX (atoi ar avea comportament nedefinit pe acele valori).
    const int64_t id = static_cast<int64_t>(std::strtoul(idText.c_str(), nullptr, 10));
    const bool started = runAsync(ctx->commandWorker, [ctx, id] {
        const bool ok = sendDeleteById(ctx, "schedule_delete", id);
        postLabel(ctx, ctx->configFeedback, ok ? "Eveniment sters" : "Stergere esuata (ID gresit?)");
    });
    setLabel(ctx->configFeedback, started ? "Se trimite..." : "Ocupat, incearca din nou");
}

lv_obj_t* addTab(lv_obj_t* tabview, const char* name) {
    auto* tab = lv_tabview_add_tab(tabview, name);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(tab, 6, 0);
    lv_obj_set_style_pad_row(tab, 6, 0);
    return tab;
}

void createWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 4, 0);

    auto* toolbar = lvgl_toolbar_create(parent, "4G Gateway");
    lvgl_toolbar_set_nav_action(toolbar, LV_SYMBOL_CLOSE, onBackPressed, ctx);

    auto* tabs = lv_tabview_create(parent);
    lv_obj_set_width(tabs, LV_PCT(100));
    lv_obj_set_flex_grow(tabs, 1);
    lv_tabview_set_tab_bar_position(tabs, LV_DIR_TOP);
    lv_tabview_set_tab_bar_size(tabs, 34);
    auto* content = addTab(tabs, "Status");
    auto* phone = addTab(tabs, "Phone");
    auto* sms = addTab(tabs, "SMS");
    auto* gnss = addTab(tabs, "GNSS");
    auto* config = addTab(tabs, "Config");

    ctx->connection = lv_label_create(content);
    lv_label_set_text(ctx->connection, "Se conecteaza la 10.1.10.12...");
    lv_obj_set_style_text_color(ctx->connection, lv_theme_get_color_primary(ctx->connection), 0);

    createCard(content, "RETEA 4G", &ctx->modem);
    ctx->quickStatus = lv_label_create(content);
    lv_label_set_text(ctx->quickStatus, "Trafic RX/TX: se incarca...");
    lv_obj_set_width(ctx->quickStatus, LV_PCT(100));
    auto* quickButtons = lv_obj_create(content);
    lv_obj_set_width(quickButtons, LV_PCT(100));
    lv_obj_set_height(quickButtons, LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(quickButtons, 0, 0);
    lv_obj_set_style_pad_all(quickButtons, 0, 0);
    lv_obj_set_flex_flow(quickButtons, LV_FLEX_FLOW_ROW_WRAP);
    addButton(quickButtons, "DATA ON", onDataOn, ctx);
    addButton(quickButtons, "DATA OFF", onDataOff, ctx);
    addButton(quickButtons, "RESTART", onModemRestart, ctx);

    // Istoric semnal (CSQ) — motorul exista deja pe gateway (300 puncte in
    // RAM), doar neexpus pe T-HMI pana acum. Actualizat mai rar decat restul
    // (vezi appMain: doar la fiecare al 4-lea tick), ca sa nu incarce refresh-ul principal.
    auto* csqLabel = lv_label_create(content);
    lv_label_set_text(csqLabel, "ISTORIC SEMNAL (CSQ)");
    lv_obj_set_style_text_color(csqLabel, lv_theme_get_color_primary(csqLabel), 0);
    ctx->csqChart = lv_chart_create(content);
    lv_obj_set_size(ctx->csqChart, LV_PCT(100), 70);
    lv_chart_set_type(ctx->csqChart, LV_CHART_TYPE_LINE);
    lv_chart_set_axis_range(ctx->csqChart, LV_CHART_AXIS_PRIMARY_Y, 0, 31);
    lv_chart_set_point_count(ctx->csqChart, 1);
    // Populat prin lv_chart_set_series_values() (tot array-ul dintr-o dată,
    // vezi refreshCsq()) — nu prin lv_chart_set_next_value(), deci modul de
    // update implicit (nu shift) e cel corect aici.
    ctx->csqSeries = lv_chart_add_series(ctx->csqChart, lv_theme_get_color_primary(ctx->csqChart), LV_CHART_AXIS_PRIMARY_Y);

    createCard(content, "SENZORI GATEWAY", &ctx->environment);
    ctx->configStatus = lv_label_create(content);
    lv_label_set_text(ctx->configStatus, "MQTT/Telegram/Automation/Geofence: se incarca...");
    lv_obj_set_width(ctx->configStatus, LV_PCT(100));

    ctx->pairStatus = lv_label_create(content);
    lv_label_set_text(ctx->pairStatus, "Open Pairing on gateway, then enter code");
    ctx->pairCode = lv_textarea_create(content);
    lv_textarea_set_one_line(ctx->pairCode, true);
    lv_textarea_set_max_length(ctx->pairCode, 6);
    lv_textarea_set_accepted_chars(ctx->pairCode, "0123456789");
    lv_textarea_set_placeholder_text(ctx->pairCode, "6-digit code");
    lv_obj_set_width(ctx->pairCode, LV_PCT(100));
    addButton(content, "Pair", onPair, ctx);

    ctx->footer = lv_label_create(content);
    lv_label_set_text(ctx->footer, "Actualizare automata la 5 secunde");
    lv_obj_set_style_text_opa(ctx->footer, LV_OPA_60, 0);

    ctx->phoneStatus = lv_label_create(phone);
    lv_label_set_text(ctx->phoneStatus, "Call idle");
    ctx->phoneNumber = lv_textarea_create(phone);
    lv_textarea_set_one_line(ctx->phoneNumber, true);
    lv_textarea_set_placeholder_text(ctx->phoneNumber, "+40...");
    lv_textarea_set_accepted_chars(ctx->phoneNumber, "+0123456789*#");
    lv_obj_set_width(ctx->phoneNumber, LV_PCT(100));
    auto* phoneButtons = lv_obj_create(phone);
    lv_obj_set_width(phoneButtons, LV_PCT(100));
    lv_obj_set_height(phoneButtons, LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(phoneButtons, 0, 0);
    lv_obj_set_style_pad_all(phoneButtons, 0, 0);
    lv_obj_set_flex_flow(phoneButtons, LV_FLEX_FLOW_ROW_WRAP);
    addButton(phoneButtons, "Dial", onDial, ctx);
    addButton(phoneButtons, "Answer", onAnswer, ctx);
    addButton(phoneButtons, "Hold", onHold, ctx);
    addButton(phoneButtons, "End", onHangup, ctx);

    ctx->smsInbox = lv_label_create(sms);
    lv_label_set_text(ctx->smsInbox, "Inbox loading...");
    lv_label_set_long_mode(ctx->smsInbox, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ctx->smsInbox, LV_PCT(100));
    ctx->smsNumber = lv_textarea_create(sms);
    lv_textarea_set_one_line(ctx->smsNumber, true);
    lv_textarea_set_placeholder_text(ctx->smsNumber, "Phone number");
    lv_textarea_set_accepted_chars(ctx->smsNumber, "+0123456789");
    lv_obj_set_width(ctx->smsNumber, LV_PCT(100));
    ctx->smsText = lv_textarea_create(sms);
    lv_textarea_set_placeholder_text(ctx->smsText, "Message");
    lv_textarea_set_max_length(ctx->smsText, 320);
    lv_obj_set_width(ctx->smsText, LV_PCT(100));
    addButton(sms, "Send SMS", onSendSms, ctx);
    addButton(sms, "Mark read", onReadSms, ctx);

    ctx->gnssStatus = lv_label_create(gnss);
    lv_label_set_text(ctx->gnssStatus, "GNSS loading...");
    addButton(gnss, "GNSS ON", onGnssOn, ctx);
    addButton(gnss, "GNSS OFF", onGnssOff, ctx);

    // Tab "Config": editare directa a setarilor de retea ale gateway-ului
    // (Wi-Fi/APN/MQTT/Telegram) + adaugare regula automation/zona geofence,
    // fara sa fie nevoie de telefon/laptop langa gateway pt WebUI. Toate
    // motoarele exista deja pe gateway — vezi node_gateway_control_post()
    // in webserver.c.
    auto* wifiLabel = lv_label_create(config);
    lv_label_set_text(wifiLabel, "WI-FI LOCAL (gateway)");
    lv_obj_set_style_text_color(wifiLabel, lv_theme_get_color_primary(wifiLabel), 0);
    ctx->staSsid = lv_textarea_create(config);
    lv_textarea_set_one_line(ctx->staSsid, true);
    lv_textarea_set_placeholder_text(ctx->staSsid, "SSID");
    lv_obj_set_width(ctx->staSsid, LV_PCT(100));
    ctx->staPass = lv_textarea_create(config);
    lv_textarea_set_one_line(ctx->staPass, true);
    lv_textarea_set_password_mode(ctx->staPass, true);
    lv_textarea_set_placeholder_text(ctx->staPass, "Parola (necompletat = neschimbata)");
    lv_obj_set_width(ctx->staPass, LV_PCT(100));
    addButton(config, "Salveaza Wi-Fi", onSaveWifi, ctx);

    auto* apnLabel = lv_label_create(config);
    lv_label_set_text(apnLabel, "APN MODEM");
    lv_obj_set_style_text_color(apnLabel, lv_theme_get_color_primary(apnLabel), 0);
    ctx->apn = lv_textarea_create(config);
    lv_textarea_set_one_line(ctx->apn, true);
    lv_textarea_set_placeholder_text(ctx->apn, "ex: net, internet");
    lv_obj_set_width(ctx->apn, LV_PCT(100));
    addButton(config, "Salveaza APN", onSaveApn, ctx);

    auto* mqttLabel = lv_label_create(config);
    lv_label_set_text(mqttLabel, "MQTT / HOME ASSISTANT");
    lv_obj_set_style_text_color(mqttLabel, lv_theme_get_color_primary(mqttLabel), 0);
    ctx->mqttUri = lv_textarea_create(config);
    lv_textarea_set_one_line(ctx->mqttUri, true);
    lv_textarea_set_placeholder_text(ctx->mqttUri, "mqtt://broker-ip:1883");
    lv_obj_set_width(ctx->mqttUri, LV_PCT(100));
    ctx->mqttUser = lv_textarea_create(config);
    lv_textarea_set_one_line(ctx->mqttUser, true);
    lv_textarea_set_placeholder_text(ctx->mqttUser, "Utilizator (optional)");
    lv_obj_set_width(ctx->mqttUser, LV_PCT(100));
    ctx->mqttPass = lv_textarea_create(config);
    lv_textarea_set_one_line(ctx->mqttPass, true);
    lv_textarea_set_password_mode(ctx->mqttPass, true);
    lv_textarea_set_placeholder_text(ctx->mqttPass, "Parola (optional)");
    lv_obj_set_width(ctx->mqttPass, LV_PCT(100));
    addButton(config, "Salveaza MQTT", onSaveMqtt, ctx);

    auto* telegramLabel = lv_label_create(config);
    lv_label_set_text(telegramLabel, "TELEGRAM");
    lv_obj_set_style_text_color(telegramLabel, lv_theme_get_color_primary(telegramLabel), 0);
    ctx->telegramToken = lv_textarea_create(config);
    lv_textarea_set_one_line(ctx->telegramToken, true);
    lv_textarea_set_placeholder_text(ctx->telegramToken, "Bot token");
    lv_obj_set_width(ctx->telegramToken, LV_PCT(100));
    ctx->telegramChat = lv_textarea_create(config);
    lv_textarea_set_one_line(ctx->telegramChat, true);
    lv_textarea_set_placeholder_text(ctx->telegramChat, "Chat ID");
    lv_obj_set_width(ctx->telegramChat, LV_PCT(100));
    addButton(config, "Salveaza Telegram", onSaveTelegram, ctx);

    auto* ruleLabel = lv_label_create(config);
    lv_label_set_text(ruleLabel, "REGULA AUTOMATION NOUA");
    lv_obj_set_style_text_color(ruleLabel, lv_theme_get_color_primary(ruleLabel), 0);
    ctx->ruleName = lv_textarea_create(config);
    lv_textarea_set_one_line(ctx->ruleName, true);
    lv_textarea_set_placeholder_text(ctx->ruleName, "Nume regula");
    lv_obj_set_width(ctx->ruleName, LV_PCT(100));
    ctx->ruleTrigger = lv_dropdown_create(config);
    lv_dropdown_set_options(ctx->ruleTrigger, "Internet jos\nSemnal (CSQ) sub prag\nBaterie sub prag");
    lv_obj_set_width(ctx->ruleTrigger, LV_PCT(100));
    ctx->ruleThreshold = lv_textarea_create(config);
    lv_textarea_set_one_line(ctx->ruleThreshold, true);
    lv_textarea_set_accepted_chars(ctx->ruleThreshold, "0123456789");
    lv_textarea_set_placeholder_text(ctx->ruleThreshold, "Prag (irelevant pt 'internet jos')");
    lv_obj_set_width(ctx->ruleThreshold, LV_PCT(100));
    ctx->ruleAction = lv_dropdown_create(config);
    lv_dropdown_set_options(ctx->ruleAction, "Alerta locala\nBroadcast catre noduri\nLED");
    lv_obj_set_width(ctx->ruleAction, LV_PCT(100));
    addButton(config, "Adauga regula", onAddRule, ctx);

    auto* ruleListTitle = lv_label_create(config);
    lv_label_set_text(ruleListTitle, "REGULI EXISTENTE");
    lv_obj_set_style_text_color(ruleListTitle, lv_theme_get_color_primary(ruleListTitle), 0);
    ctx->ruleList = lv_label_create(config);
    lv_label_set_text(ctx->ruleList, "Se incarca...");
    lv_label_set_long_mode(ctx->ruleList, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ctx->ruleList, LV_PCT(100));
    ctx->ruleDeleteId = lv_textarea_create(config);
    lv_textarea_set_one_line(ctx->ruleDeleteId, true);
    lv_textarea_set_accepted_chars(ctx->ruleDeleteId, "0123456789");
    lv_textarea_set_placeholder_text(ctx->ruleDeleteId, "ID regula de sters (vezi # din lista)");
    lv_obj_set_width(ctx->ruleDeleteId, LV_PCT(100));
    addButton(config, "Sterge regula", onDeleteRule, ctx);

    auto* geoLabel = lv_label_create(config);
    lv_label_set_text(geoLabel, "ZONA GEOFENCE NOUA (pozitia curenta gateway)");
    lv_obj_set_style_text_color(geoLabel, lv_theme_get_color_primary(geoLabel), 0);
    ctx->geoName = lv_textarea_create(config);
    lv_textarea_set_one_line(ctx->geoName, true);
    lv_textarea_set_placeholder_text(ctx->geoName, "Nume zona (ex: Acasa)");
    lv_obj_set_width(ctx->geoName, LV_PCT(100));
    ctx->geoRadius = lv_textarea_create(config);
    lv_textarea_set_one_line(ctx->geoRadius, true);
    lv_textarea_set_accepted_chars(ctx->geoRadius, "0123456789");
    lv_textarea_set_placeholder_text(ctx->geoRadius, "Raza in metri (ex: 100)");
    lv_obj_set_width(ctx->geoRadius, LV_PCT(100));
    addButton(config, "Adauga zona (necesita fix GNSS)", onAddGeofence, ctx);

    auto* geoListTitle = lv_label_create(config);
    lv_label_set_text(geoListTitle, "ZONE EXISTENTE");
    lv_obj_set_style_text_color(geoListTitle, lv_theme_get_color_primary(geoListTitle), 0);
    ctx->geoList = lv_label_create(config);
    lv_label_set_text(ctx->geoList, "Se incarca...");
    lv_label_set_long_mode(ctx->geoList, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ctx->geoList, LV_PCT(100));
    ctx->geoDeleteIdx = lv_textarea_create(config);
    lv_textarea_set_one_line(ctx->geoDeleteIdx, true);
    lv_textarea_set_accepted_chars(ctx->geoDeleteIdx, "0123456789");
    lv_textarea_set_placeholder_text(ctx->geoDeleteIdx, "Index zona de sters (vezi # din lista)");
    lv_obj_set_width(ctx->geoDeleteIdx, LV_PCT(100));
    addButton(config, "Sterge zona", onDeleteGeofence, ctx);

    // Host watchdog: motorul exista deja pe gateway (host_watchdog.c),
    // configurabil doar din WebUI pana acum.
    auto* watchdogTitle = lv_label_create(config);
    lv_label_set_text(watchdogTitle, "MONITOR DISPONIBILITATE (HOST WATCHDOG)");
    lv_obj_set_style_text_color(watchdogTitle, lv_theme_get_color_primary(watchdogTitle), 0);
    ctx->watchdogStatus = lv_label_create(config);
    lv_label_set_text(ctx->watchdogStatus, "Se incarca...");
    lv_obj_set_width(ctx->watchdogStatus, LV_PCT(100));
    auto* watchdogEnableRow = lv_obj_create(config);
    lv_obj_set_width(watchdogEnableRow, LV_PCT(100));
    lv_obj_set_height(watchdogEnableRow, LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(watchdogEnableRow, 0, 0);
    lv_obj_set_style_bg_opa(watchdogEnableRow, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(watchdogEnableRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(watchdogEnableRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    auto* watchdogEnableLabel = lv_label_create(watchdogEnableRow);
    lv_label_set_text(watchdogEnableLabel, "Activ");
    ctx->watchdogEnabledSwitch = lv_switch_create(watchdogEnableRow);
    ctx->watchdogHost = lv_textarea_create(config);
    lv_textarea_set_one_line(ctx->watchdogHost, true);
    lv_textarea_set_placeholder_text(ctx->watchdogHost, "Host/IP de monitorizat");
    lv_obj_set_width(ctx->watchdogHost, LV_PCT(100));
    ctx->watchdogPort = lv_textarea_create(config);
    lv_textarea_set_one_line(ctx->watchdogPort, true);
    lv_textarea_set_accepted_chars(ctx->watchdogPort, "0123456789");
    lv_textarea_set_placeholder_text(ctx->watchdogPort, "Port TCP (ex: 443)");
    lv_obj_set_width(ctx->watchdogPort, LV_PCT(100));
    ctx->watchdogInterval = lv_textarea_create(config);
    lv_textarea_set_one_line(ctx->watchdogInterval, true);
    lv_textarea_set_accepted_chars(ctx->watchdogInterval, "0123456789");
    lv_textarea_set_placeholder_text(ctx->watchdogInterval, "Interval verificare (secunde, ex: 60)");
    lv_obj_set_width(ctx->watchdogInterval, LV_PCT(100));
    ctx->watchdogFailures = lv_textarea_create(config);
    lv_textarea_set_one_line(ctx->watchdogFailures, true);
    lv_textarea_set_accepted_chars(ctx->watchdogFailures, "0123456789");
    lv_textarea_set_placeholder_text(ctx->watchdogFailures, "Esecuri pana la alarma (ex: 3)");
    lv_obj_set_width(ctx->watchdogFailures, LV_PCT(100));
    ctx->watchdogSmsTo = lv_textarea_create(config);
    lv_textarea_set_one_line(ctx->watchdogSmsTo, true);
    lv_textarea_set_accepted_chars(ctx->watchdogSmsTo, "+0123456789");
    lv_textarea_set_placeholder_text(ctx->watchdogSmsTo, "Numar SMS alerta");
    lv_obj_set_width(ctx->watchdogSmsTo, LV_PCT(100));
    addButton(config, "Salveaza watchdog", onSaveWatchdog, ctx);

    // Scheduler (calendar): motorul exista deja pe gateway (scheduler.c —
    // SMS programate, o data/zilnic/saptamanal), configurabil doar din WebUI
    // pana acum.
    auto* scheduleTitle = lv_label_create(config);
    lv_label_set_text(scheduleTitle, "EVENIMENTE PROGRAMATE EXISTENTE");
    lv_obj_set_style_text_color(scheduleTitle, lv_theme_get_color_primary(scheduleTitle), 0);
    ctx->scheduleList = lv_label_create(config);
    lv_label_set_text(ctx->scheduleList, "Se incarca...");
    lv_label_set_long_mode(ctx->scheduleList, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ctx->scheduleList, LV_PCT(100));
    ctx->scheduleDeleteId = lv_textarea_create(config);
    lv_textarea_set_one_line(ctx->scheduleDeleteId, true);
    lv_textarea_set_accepted_chars(ctx->scheduleDeleteId, "0123456789");
    lv_textarea_set_placeholder_text(ctx->scheduleDeleteId, "ID eveniment de sters (vezi # din lista)");
    lv_obj_set_width(ctx->scheduleDeleteId, LV_PCT(100));
    addButton(config, "Sterge eveniment", onDeleteScheduleEvent, ctx);

    auto* scheduleAddTitle = lv_label_create(config);
    lv_label_set_text(scheduleAddTitle, "EVENIMENT NOU");
    lv_obj_set_style_text_color(scheduleAddTitle, lv_theme_get_color_primary(scheduleAddTitle), 0);
    ctx->scheduleLabel = lv_textarea_create(config);
    lv_textarea_set_one_line(ctx->scheduleLabel, true);
    lv_textarea_set_placeholder_text(ctx->scheduleLabel, "Nume eveniment");
    lv_obj_set_width(ctx->scheduleLabel, LV_PCT(100));
    ctx->scheduleMinutes = lv_textarea_create(config);
    lv_textarea_set_one_line(ctx->scheduleMinutes, true);
    lv_textarea_set_accepted_chars(ctx->scheduleMinutes, "0123456789");
    lv_textarea_set_placeholder_text(ctx->scheduleMinutes, "Peste cate minute (necesita ceas T-HMI sincronizat)");
    lv_obj_set_width(ctx->scheduleMinutes, LV_PCT(100));
    ctx->scheduleRepeat = lv_dropdown_create(config);
    lv_dropdown_set_options(ctx->scheduleRepeat, "O singura data\nZilnic\nSaptamanal");
    lv_obj_set_width(ctx->scheduleRepeat, LV_PCT(100));
    ctx->scheduleSmsTo = lv_textarea_create(config);
    lv_textarea_set_one_line(ctx->scheduleSmsTo, true);
    lv_textarea_set_accepted_chars(ctx->scheduleSmsTo, "+0123456789");
    lv_textarea_set_placeholder_text(ctx->scheduleSmsTo, "Numar SMS destinatie");
    lv_obj_set_width(ctx->scheduleSmsTo, LV_PCT(100));
    ctx->scheduleSmsText = lv_textarea_create(config);
    lv_textarea_set_placeholder_text(ctx->scheduleSmsText, "Text SMS");
    lv_textarea_set_max_length(ctx->scheduleSmsText, 120);
    lv_obj_set_width(ctx->scheduleSmsText, LV_PCT(100));
    addButton(config, "Adauga eveniment", onAddScheduleEvent, ctx);

    ctx->configFeedback = lv_label_create(config);
    lv_label_set_text(ctx->configFeedback, "");
    lv_obj_set_style_text_opa(ctx->configFeedback, LV_OPA_70, 0);
}

int32_t appMain(int, char**) {
    Context ctx {};
    ctx.appInstanceId = app_scheduler_current_app_id();

    TaskEventGroup eventGroup {};
    task_event_group_construct(&eventGroup);
    AppEventSubscription subscription {};
    check(app_event_subscribe(&subscription, &eventGroup) == ERROR_NONE);

    initIdentity(&ctx);
    ctx.paired = loadCredentials(&ctx);
    const WindowId window = window_manager_create(ctx.appInstanceId, createWidgets, &ctx);
    ctx.window = window;
    lvgl_lock();
    setLabel(ctx.pairStatus, ctx.paired ? "PAIR OK - secure link active" : "Not paired - enter the gateway code");
    lvgl_unlock();
    ctx.refreshTimer = std::make_unique<Timer>(Timer::Type::Periodic, millis_to_ticks(5000), [&ctx] { refreshAsync(&ctx); });
    ctx.refreshTimer->start();
    refreshAsync(&ctx);
    // Istoric CSQ: mult mai rar decat refresh-ul principal — e doar un grafic
    // de tendinta, nu are nevoie de cadenta de 5s.
    ctx.csqTimer = std::make_unique<Timer>(Timer::Type::Periodic, millis_to_ticks(20000), [&ctx] { refreshCsqAsync(&ctx); });
    ctx.csqTimer->start();
    refreshCsqAsync(&ctx);

    bool shouldClose = false;
    while (!shouldClose) {
        task_event_group_wait_any(&eventGroup, nullptr, portMAX_DELAY);
        AppEvent event {};
        while (app_event_poll(&subscription, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_CLOSE) shouldClose = true;
        }
    }

    ctx.refreshTimer->stop();
    ctx.csqTimer->stop();
    stopWorker(ctx.refreshWorker);
    stopWorker(ctx.commandWorker);
    stopWorker(ctx.csqWorker);
    window_manager_remove(window);
    check(app_event_unsubscribe(&subscription) == ERROR_NONE);
    task_event_group_destruct(&eventGroup);
    return 0;
}

} // namespace

extern const ::AppManifest manifest = {
    .id = "tactility.gateway4g",
    .name = "4G Gateway",
    .category = APP_CATEGORY_USER,
    .location = { APP_LOCATION_MEMORY, reinterpret_cast<void*>(appMain) },
    .flags = 0,
    .stack = { .depth = 6144, .desired_memory_capability = MEMORY_CAPABILITY_EXTERNAL }
};

} // namespace tt::app::gateway4g
