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
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace tt::app::gateway4g {

namespace {

constexpr auto* GATEWAY_URL = "http://10.1.10.12/health";

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
    uint64_t txBytes = 0;
    uint64_t rxBytes = 0;
    std::string smsPreview;
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
    std::string nodeId;
    std::string key;
    bool paired = false;
    std::mutex credLock;
    WindowId window = 0;
    std::unique_ptr<Timer> refreshTimer;
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

bool sendCommand(Context* ctx, const char* command, const char* value = nullptr, const char* text = nullptr) {
    const auto identity = creds(ctx);
    if (!identity.paired) return false;
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "command", command);
    if (value) cJSON_AddStringToObject(json, "value", value);
    if (text) cJSON_AddStringToObject(json, "text", text);
    char* encoded = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!encoded) return false;
    std::string response;
    const bool ok = request("http://10.1.10.12/api/node/control", HTTP_METHOD_POST,
        encoded, identity.nodeId, identity.key, response, 30000);
    cJSON_free(encoded);
    return ok;
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
    data.txBytes = static_cast<uint64_t>(jsonDouble(root, "tx_bytes"));
    data.rxBytes = static_cast<uint64_t>(jsonDouble(root, "rx_bytes"));
    auto* sms = cJSON_GetObjectItemCaseSensitive(root, "sms");
    if (cJSON_IsArray(sms) && cJSON_GetArraySize(sms) > 0) {
        auto* first = cJSON_GetArrayItem(sms, 0);
        data.smsPreview = jsonString(first, "number") + ": " + jsonString(first, "text");
    }
    cJSON_Delete(root);
    return true;
}
#else
void initIdentity(Context*) {}
bool loadCredentials(Context*) { return false; }
bool pairGateway(Context*, const std::string&) { return false; }
bool sendCommand(Context*, const char*, const char* = nullptr, const char* = nullptr) { return false; }
bool fetchGateway(Context*, GatewayData&) { return false; }
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

    if (data.txBytes || data.rxBytes) {
        std::snprintf(line, sizeof(line), "RX %.1f MB   TX %.1f MB",
            data.rxBytes / 1048576.0, data.txBytes / 1048576.0);
    } else {
        std::snprintf(line, sizeof(line), "Trafic RX/TX: necesita update gateway");
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
    createCard(content, "SENZORI GATEWAY", &ctx->environment);

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

    bool shouldClose = false;
    while (!shouldClose) {
        task_event_group_wait_any(&eventGroup, nullptr, portMAX_DELAY);
        AppEvent event {};
        while (app_event_poll(&subscription, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_CLOSE) shouldClose = true;
        }
    }

    ctx.refreshTimer->stop();
    stopWorker(ctx.refreshWorker);
    stopWorker(ctx.commandWorker);
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
