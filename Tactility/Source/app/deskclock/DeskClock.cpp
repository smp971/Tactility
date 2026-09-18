#include <app/event.h>
#include <app/manifest.h>
#include <app/scheduler.h>
#include <lvgl/lvgl.h>
#include <lvgl/fonts.h>
#include <lvgl/icons/shared.h>
#include <lvgl_window_manager/window_manager.h>
#include <lvgl/widgets/toolbar.h>
#include <tactility/check.h>
#include <tactility/memory.h>
#include <tactility/time.h>
#include <Tactility/Tactility.h>
#include <Tactility/Timer.h>

#ifdef ESP_PLATFORM
#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#endif

#include <ctime>
#include <cmath>
#include <memory>
#include <string>

namespace tt::app::deskclock {
namespace {

constexpr auto* FORECAST_URL = "https://api.open-meteo.com/v1/forecast?latitude=44.4323&longitude=26.1063&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,is_day&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max&timezone=Europe%2FBucharest&forecast_days=3";
constexpr auto* GATEWAY_URL = "http://10.1.10.12/health";

struct Weather {
    bool forecastOk = false;
    bool indoorOk = false;
    double outdoor = 0;
    double apparent = 0;
    double indoor = 0;
    double humidity = 0;
    double indoorHumidity = 0;
    double pressure = 0;
    double wind = 0;
    double max = 0;
    double min = 0;
    int rain = 0;
    int code = -1;
    bool isDay = true;
    int dailyCode[3] {-1, -1, -1};
    double dailyMax[3] {};
    double dailyMin[3] {};
    int dailyRain[3] {};
};

struct Context {
    uint32_t appInstanceId = 0;
    lv_obj_t* clock = nullptr;
    lv_obj_t* date = nullptr;
    lv_obj_t* content = nullptr;
    lv_obj_t* currentCard = nullptr;
    lv_obj_t* currentIcon = nullptr;
    lv_obj_t* condition = nullptr;
    lv_obj_t* outdoor = nullptr;
    lv_obj_t* metricCards[3] {};
    lv_obj_t* metricLabels[3] {};
    lv_obj_t* footer = nullptr;
    Weather weather;
    std::unique_ptr<Timer> clockTimer;
    std::unique_ptr<Timer> weatherTimer;
};

#ifdef ESP_PLATFORM
esp_err_t httpEvent(esp_http_client_event_t* event) {
    if (event->event_id == HTTP_EVENT_ON_DATA && event->user_data && event->data_len > 0)
        static_cast<std::string*>(event->user_data)->append(static_cast<const char*>(event->data), event->data_len);
    return ESP_OK;
}

bool get(const char* url, bool https, std::string& output) {
    esp_http_client_config_t config {};
    config.url = url;
    config.timeout_ms = 7000;
    config.buffer_size = 2048;
    config.event_handler = httpEvent;
    config.user_data = &output;
    if (https) config.crt_bundle_attach = esp_crt_bundle_attach;
    auto* client = esp_http_client_init(&config);
    if (!client) return false;
    auto result = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return result == ESP_OK && status == 200;
}

double number(cJSON* object, const char* key, double fallback = 0) {
    auto* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

double firstNumber(cJSON* object, const char* key) {
    auto* array = cJSON_GetObjectItemCaseSensitive(object, key);
    auto* item = cJSON_IsArray(array) ? cJSON_GetArrayItem(array, 0) : nullptr;
    return cJSON_IsNumber(item) ? item->valuedouble : 0;
}

double arrayNumber(cJSON* object, const char* key, int index, double fallback = 0) {
    auto* array = cJSON_GetObjectItemCaseSensitive(object, key);
    auto* item = cJSON_IsArray(array) ? cJSON_GetArrayItem(array, index) : nullptr;
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

void fetchWeather(Weather& weather) {
    std::string body;
    if (get(FORECAST_URL, true, body)) {
        auto* root = cJSON_Parse(body.c_str());
        auto* current = root ? cJSON_GetObjectItemCaseSensitive(root, "current") : nullptr;
        auto* daily = root ? cJSON_GetObjectItemCaseSensitive(root, "daily") : nullptr;
        if (cJSON_IsObject(current) && cJSON_IsObject(daily)) {
            weather.outdoor = number(current, "temperature_2m");
            weather.apparent = number(current, "apparent_temperature");
            weather.humidity = number(current, "relative_humidity_2m");
            weather.wind = number(current, "wind_speed_10m");
            weather.code = static_cast<int>(number(current, "weather_code", -1));
            weather.isDay = number(current, "is_day", 1) != 0;
            weather.max = firstNumber(daily, "temperature_2m_max");
            weather.min = firstNumber(daily, "temperature_2m_min");
            weather.rain = static_cast<int>(firstNumber(daily, "precipitation_probability_max"));
            for (int i = 0; i < 3; i++) {
                weather.dailyCode[i] = static_cast<int>(arrayNumber(daily, "weather_code", i, -1));
                weather.dailyMax[i] = arrayNumber(daily, "temperature_2m_max", i);
                weather.dailyMin[i] = arrayNumber(daily, "temperature_2m_min", i);
                weather.dailyRain[i] = static_cast<int>(arrayNumber(daily, "precipitation_probability_max", i));
            }
            weather.forecastOk = true;
        }
        cJSON_Delete(root);
    }
    body.clear();
    if (get(GATEWAY_URL, false, body)) {
        auto* root = cJSON_Parse(body.c_str());
        if (root) {
            weather.indoor = number(root, "bmp_temp_c");
            weather.indoorHumidity = number(root, "bmp_humidity");
            weather.pressure = number(root, "bmp_pressure_hpa");
            weather.indoorOk = true;
        }
        cJSON_Delete(root);
    }
}
#else
void fetchWeather(Weather&) {}
#endif

const char* description(int code) {
    if (code == 0) return "Senin";
    if (code <= 3) return "Partial noros";
    if (code == 45 || code == 48) return "Ceata";
    if (code >= 51 && code <= 67) return "Ploaie";
    if (code >= 71 && code <= 77) return "Ninsoare";
    if (code >= 80 && code <= 82) return "Averse";
    if (code >= 95) return "Furtuna";
    return "Meteo indisponibil";
}

const char* weatherIcon(int code) {
    if (code == 0) return LVGL_ICON_SHARED_LIGHTBULB;
    if (code >= 95) return LVGL_ICON_SHARED_ELECTRIC_BOLT;
    return LVGL_ICON_SHARED_CLOUD;
}

lv_color_t weatherColor(int code, bool isDay) {
    if (code == 0) return lv_color_hex(isDay ? 0xFFD54F : 0x90CAF9);
    if (code >= 51 && code <= 82) return lv_color_hex(0x42A5F5);
    if (code >= 95) return lv_color_hex(0xFFCA28);
    if (code >= 71 && code <= 77) return lv_color_hex(0xE3F2FD);
    return lv_color_hex(0xB0BEC5);
}

void applyTheme(Context* ctx) {
    const auto background = lv_color_hex(0x101827);
    const auto card = lv_color_hex(0x1E293B);
    const auto text = lv_color_hex(0xEAF2FF);
    const auto muted = lv_color_hex(0x94A3B8);
    lv_obj_set_style_bg_color(ctx->content, background, 0);
    lv_obj_set_style_text_color(ctx->content, text, 0);
    lv_obj_set_style_bg_color(ctx->currentCard, card, 0);
    for (auto* metricCard : ctx->metricCards) lv_obj_set_style_bg_color(metricCard, card, 0);
    lv_obj_set_style_text_color(ctx->clock, lv_color_hex(0x64B5F6), 0);
    lv_obj_set_style_text_color(ctx->footer, muted, 0);
}

struct Decimal1 {
    const char* sign;
    int whole;
    int fraction;
};

Decimal1 decimal1(double value) {
    const int scaled = static_cast<int>(std::lround(value * 10.0));
    const int absolute = std::abs(scaled);
    return { scaled < 0 ? "-" : "", absolute / 10, absolute % 10 };
}

void updateClock(Context* ctx) {
    static const char* days[] = {"Duminica", "Luni", "Marti", "Miercuri", "Joi", "Vineri", "Sambata"};
    static const char* months[] = {"ianuarie", "februarie", "martie", "aprilie", "mai", "iunie", "iulie", "august", "septembrie", "octombrie", "noiembrie", "decembrie"};
    std::time_t now = std::time(nullptr);
    std::tm local {};
    localtime_r(&now, &local);
    char timeText[16];
    std::strftime(timeText, sizeof(timeText), "%H:%M:%S", &local);
    lv_label_set_text(ctx->clock, timeText);
    lv_label_set_text_fmt(ctx->date, "%s, %d %s", days[local.tm_wday], local.tm_mday, months[local.tm_mon]);
}

void renderWeather(Context* ctx) {
    const auto& w = ctx->weather;
    lv_label_set_text(ctx->currentIcon, weatherIcon(w.code));
    lv_obj_set_style_text_color(ctx->currentIcon, weatherColor(w.code, w.isDay), 0);
    lv_label_set_text_fmt(ctx->condition, "%s | Bucuresti", description(w.code));
    if (w.forecastOk) {
        const auto outdoor = decimal1(w.outdoor);
        const auto apparent = decimal1(w.apparent);
        lv_label_set_text_fmt(ctx->outdoor, "%s%d.%d C | R %s%d.%d C | V %d km/h",
            outdoor.sign, outdoor.whole, outdoor.fraction, apparent.sign, apparent.whole, apparent.fraction,
            static_cast<int>(std::lround(w.wind)));
    } else lv_label_set_text(ctx->outdoor, "Prognoza online indisponibila");
    if (w.indoorOk) {
        const auto indoor = decimal1(w.indoor);
        lv_label_set_text_fmt(ctx->metricLabels[0], "TEMP\n%s%d.%d C", indoor.sign, indoor.whole, indoor.fraction);
        lv_label_set_text_fmt(ctx->metricLabels[1], "UMIDITATE\n%d %%", static_cast<int>(std::lround(w.indoorHumidity)));
        lv_label_set_text_fmt(ctx->metricLabels[2], "PRESIUNE\n%d hPa", static_cast<int>(std::lround(w.pressure)));
    } else {
        lv_label_set_text(ctx->metricLabels[0], "TEMP\n--");
        lv_label_set_text(ctx->metricLabels[1], "UMIDITATE\n--");
        lv_label_set_text(ctx->metricLabels[2], "PRESIUNE\n--");
    }
    lv_label_set_text(ctx->footer, "Open-Meteo | dark mode");
    applyTheme(ctx);
}

void refreshWeather(Context* ctx) {
    Weather next;
    fetchWeather(next);
    lvgl_lock();
    ctx->weather = next;
    renderWeather(ctx);
    lvgl_unlock();
}

void onBack(lv_event_t* event) {
    app_event_emit_close(static_cast<Context*>(lv_event_get_user_data(event))->appInstanceId);
}

void createWidgets(lv_obj_t* parent, void* data) {
    auto* ctx = static_cast<Context*>(data);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 0, 0);
    auto* toolbar = lvgl_toolbar_create(parent, "Desk Clock");
    lvgl_toolbar_set_nav_action(toolbar, LV_SYMBOL_CLOSE, onBack, ctx);
    auto* content = lv_obj_create(parent);
    ctx->content = content;
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 4, 0);
    lv_obj_set_style_pad_row(content, 2, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ctx->clock = lv_label_create(content);
    lv_obj_set_style_text_font(ctx->clock, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(ctx->clock, lv_theme_get_color_primary(ctx->clock), 0);
    ctx->date = lv_label_create(content);
    ctx->currentCard = lv_obj_create(content);
    lv_obj_set_size(ctx->currentCard, LV_PCT(96), 50);
    lv_obj_set_style_margin_top(ctx->currentCard, 10, 0);
    lv_obj_set_style_radius(ctx->currentCard, 10, 0);
    lv_obj_set_style_border_width(ctx->currentCard, 0, 0);
    lv_obj_set_style_pad_all(ctx->currentCard, 4, 0);
    lv_obj_set_flex_flow(ctx->currentCard, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctx->currentCard, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ctx->currentIcon = lv_label_create(ctx->currentCard);
    lv_obj_set_style_text_font(ctx->currentIcon, lvgl_get_shared_icon_font(), 0);
    auto* currentText = lv_obj_create(ctx->currentCard);
    lv_obj_set_style_bg_opa(currentText, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(currentText, 0, 0);
    lv_obj_set_style_pad_all(currentText, 0, 0);
    lv_obj_set_flex_grow(currentText, 1);
    lv_obj_set_height(currentText, LV_PCT(100));
    lv_obj_set_flex_flow(currentText, LV_FLEX_FLOW_COLUMN);
    ctx->condition = lv_label_create(currentText);
    lv_obj_set_style_text_font(ctx->condition, lvgl_get_text_font(FONT_SIZE_DEFAULT), 0);
    ctx->outdoor = lv_label_create(currentText);
    lv_obj_set_style_text_align(ctx->outdoor, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(ctx->outdoor, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
    lv_label_set_long_mode(ctx->outdoor, LV_LABEL_LONG_CLIP);
    auto* metricsRow = lv_obj_create(content);
    lv_obj_set_size(metricsRow, LV_PCT(96), 56);
    lv_obj_set_style_bg_opa(metricsRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(metricsRow, 0, 0);
    lv_obj_set_style_pad_all(metricsRow, 0, 0);
    lv_obj_set_style_pad_column(metricsRow, 4, 0);
    lv_obj_set_flex_flow(metricsRow, LV_FLEX_FLOW_ROW);
    for (int i = 0; i < 3; i++) {
        ctx->metricCards[i] = lv_obj_create(metricsRow);
        lv_obj_set_height(ctx->metricCards[i], LV_PCT(100));
        lv_obj_set_flex_grow(ctx->metricCards[i], 1);
        lv_obj_set_style_radius(ctx->metricCards[i], 9, 0);
        lv_obj_set_style_border_width(ctx->metricCards[i], 0, 0);
        lv_obj_set_style_pad_all(ctx->metricCards[i], 3, 0);
        ctx->metricLabels[i] = lv_label_create(ctx->metricCards[i]);
        lv_obj_center(ctx->metricLabels[i]);
        lv_obj_set_style_text_align(ctx->metricLabels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(ctx->metricLabels[i], lvgl_get_text_font(FONT_SIZE_SMALL), 0);
    }
    ctx->footer = lv_label_create(content);
    lv_obj_set_style_text_opa(ctx->footer, LV_OPA_50, 0);
    updateClock(ctx);
    renderWeather(ctx);
}

int32_t appMain(int, char**) {
    Context ctx {};
    ctx.appInstanceId = app_scheduler_current_app_id();
    TaskEventGroup events {};
    task_event_group_construct(&events);
    AppEventSubscription subscription {};
    check(app_event_subscribe(&subscription, &events) == ERROR_NONE);
    WindowId window = window_manager_create(ctx.appInstanceId, createWidgets, &ctx);
    ctx.clockTimer = std::make_unique<Timer>(Timer::Type::Periodic, seconds_to_ticks(1), [&ctx] {
        lvgl_lock(); updateClock(&ctx); lvgl_unlock();
    });
    ctx.weatherTimer = std::make_unique<Timer>(Timer::Type::Periodic, seconds_to_ticks(900), [&ctx] { refreshWeather(&ctx); });
    ctx.clockTimer->start();
    ctx.weatherTimer->start();
    refreshWeather(&ctx);
    bool close = false;
    while (!close) {
        task_event_group_wait_any(&events, nullptr, portMAX_DELAY);
        AppEvent event {};
        while (app_event_poll(&subscription, &event) == ERROR_NONE)
            if (event.type == APP_EVENT_CLOSE) close = true;
    }
    ctx.clockTimer->stop();
    ctx.weatherTimer->stop();
    window_manager_remove(window);
    check(app_event_unsubscribe(&subscription) == ERROR_NONE);
    task_event_group_destruct(&events);
    return 0;
}
} // namespace

extern const ::AppManifest manifest = {
    .id = "tactility.deskclock", .name = "Desk Clock", .category = APP_CATEGORY_USER,
    .location = { APP_LOCATION_MEMORY, reinterpret_cast<void*>(appMain) }, .flags = 0,
    .stack = { .depth = 6144, .desired_memory_capability = MEMORY_CAPABILITY_EXTERNAL }
};
} // namespace tt::app::deskclock
