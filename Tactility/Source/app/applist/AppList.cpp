#include <app/event.h>
#include <app/manager.h>
#include <app/manifest.h>
#include <app/scheduler.h>
#include <app/start.h>

#include <lvgl_window_manager/window_manager.h>

#include <tactility/check.h>

#include <lvgl.h>
#include <algorithm>
#include <cstring>
#include <vector>

#include <lvgl/icons/shared.h>
#include <lvgl/fonts.h>
#include <lvgl/widgets/toolbar.h>

namespace tt::app::applist {

namespace {

struct Context {
    uint32_t appInstanceId;
};

void onAppPressed(lv_event_t* e) {
    // Fire-and-forget top-level navigation, same as Launcher's own app-launch buttons.
    const auto* manifest = static_cast<const ::AppManifest*>(lv_event_get_user_data(e));
    uint32_t instanceId = 0;
    app_start(manifest->id, 0, nullptr, &instanceId);
}

void onBackPressed(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    app_event_emit_close(ctx->appInstanceId);
}

const char* appIcon(const char* id) {
    if (strstr(id, "chat")) return LVGL_ICON_SHARED_FORUM;
    if (strstr(id, "deskclock") || strstr(id, "time")) return LVGL_ICON_SHARED_CALENDAR_MONTH;
    if (strstr(id, "gateway")) return LVGL_ICON_SHARED_HUB;
    if (strstr(id, "gps")) return LVGL_ICON_SHARED_NAVIGATION;
    if (strstr(id, "i2c") || strstr(id, "grove")) return LVGL_ICON_SHARED_CABLE;
    if (strstr(id, "wifi")) return LVGL_ICON_SHARED_WIFI;
    if (strstr(id, "bt")) return LVGL_ICON_SHARED_BLUETOOTH;
    if (strstr(id, "usb")) return LVGL_ICON_SHARED_USB;
    if (strstr(id, "audio")) return LVGL_ICON_SHARED_MUSIC_NOTE;
    if (strstr(id, "keyboard")) return LVGL_ICON_SHARED_KEYBOARD_ALT;
    if (strstr(id, "display")) return LVGL_ICON_SHARED_DISPLAY_SETTINGS;
    if (strstr(id, "power")) return LVGL_ICON_SHARED_POWER_SETTINGS_NEW;
    if (strstr(id, "notes")) return LVGL_ICON_SHARED_EDIT_NOTE;
    if (strstr(id, "image") || strstr(id, "screenshot")) return LVGL_ICON_SHARED_IMAGE;
    if (strstr(id, "files") || strstr(id, "fileselection")) return LVGL_ICON_SHARED_FOLDER;
    if (strstr(id, "webserver") || strstr(id, "locale")) return LVGL_ICON_SHARED_LANGUAGE;
    if (strstr(id, "development")) return LVGL_ICON_SHARED_DEPLOYED_CODE;
    if (strstr(id, "systeminfo")) return LVGL_ICON_SHARED_DEVICES;
    if (strstr(id, "crash")) return LVGL_ICON_SHARED_HELP;
    if (strstr(id, "apphub") || strstr(id, "package")) return LVGL_ICON_SHARED_DOWNLOAD;
    if (strstr(id, "settings") || strstr(id, "setup")) return LVGL_ICON_SHARED_SETTINGS;
    if (strstr(id, "trackball")) return LVGL_ICON_SHARED_GAMEPAD;
    if (strstr(id, "touch")) return LVGL_ICON_SHARED_CIRCLE;
    return LVGL_ICON_SHARED_TOOLBAR;
}

void createAppWidget(const ::AppManifest* manifest, lv_obj_t* list) {
    lv_obj_t* btn = lv_list_add_button(list, appIcon(manifest->id), manifest->name);
    lv_obj_t* image = lv_obj_get_child(btn, 0);
    lv_obj_set_style_text_font(image, lvgl_get_shared_icon_font(), LV_PART_MAIN);
    lv_obj_add_event_cb(btn, &onAppPressed, LV_EVENT_SHORT_CLICKED, const_cast<::AppManifest*>(manifest));
}

void collectManifest(const ::AppManifest* manifest, void* context) {
    auto* manifests = static_cast<std::vector<const ::AppManifest*>*>(context);
    manifests->push_back(manifest);
}

void createWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    // Flex column + flex_grow so LVGL recomputes the toolbar/list split on every layout pass,
    // rather than a fixed height computed once from lv_obj_get_content_height(parent) that would
    // go stale after a later display resolution/rotation change.
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 0, LV_STATE_DEFAULT);

    auto* toolbar = lvgl_toolbar_create(parent, "Apps");
    lvgl_toolbar_set_nav_action(toolbar, LV_SYMBOL_CLOSE, onBackPressed, ctx);

    lv_obj_t* list = lv_list_create(parent);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);

    std::vector<const ::AppManifest*> manifests;
    app_manager_for_each_manifest(collectManifest, &manifests);
    std::ranges::sort(manifests, [](const ::AppManifest* a, const ::AppManifest* b) {
        return strcmp(a->name, b->name) < 0;
    });

    for (const auto* manifest: manifests) {
        bool is_valid_category = (manifest->category == APP_CATEGORY_USER) || (manifest->category == APP_CATEGORY_SYSTEM);
        if (is_valid_category && (manifest->flags & APP_MANIFEST_FLAG_HIDDEN) == 0) {
            createAppWidget(manifest, list);
        }
    }
}

int32_t appMain(int argc, char* argv[]) {
    uint32_t appInstanceId = app_scheduler_current_app_id();
    Context ctx { appInstanceId };

    TaskEventGroup event_group {};
    task_event_group_construct(&event_group);

    AppEventSubscription sub {};
    check(app_event_subscribe(&sub, &event_group) == ERROR_NONE);

    WindowId window = window_manager_create(appInstanceId, createWidgets, &ctx);

    while (true) {
        task_event_group_wait_any(&event_group, nullptr, portMAX_DELAY);

        bool shouldClose = false;
        AppEvent event {};
        while (app_event_poll(&sub, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_CLOSE) {
                shouldClose = true;
                break;
            }
        }
        if (shouldClose) break;
    }

    window_manager_remove(window);
    check(app_event_unsubscribe(&sub) == ERROR_NONE);
    task_event_group_destruct(&event_group);
    return 0;
}

} // namespace

extern const ::AppManifest manifest = {
    .id = "tactility.applist",
    .name = "Apps",
    .category = APP_CATEGORY_SYSTEM,
    .location = { .type = APP_LOCATION_MEMORY, .location = reinterpret_cast<void*>(appMain) },
    .flags = APP_MANIFEST_FLAG_HIDDEN,
    .stack = { .depth = 2400, .desired_memory_capability = 0 },
};

} // namespace
