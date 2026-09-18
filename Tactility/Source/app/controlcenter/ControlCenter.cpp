#include <app/event.h>
#include <app/manager.h>
#include <app/manifest.h>
#include <app/scheduler.h>
#include <app/start.h>

#include <lvgl.h>
#include <lvgl/fonts.h>
#include <lvgl/icons/launcher.h>
#include <lvgl_window_manager/window_manager.h>

#include <tactility/check.h>
#include <tactility/memory.h>

namespace tt::app::controlcenter {

namespace {

struct Shortcut {
    const char* symbol;
    const char* label;
    const char* appId;
};

constexpr Shortcut SHORTCUTS[] = {
    { LVGL_ICON_LAUNCHER_APPS, "Apps", "tactility.applist" },
    { LVGL_ICON_LAUNCHER_FOLDER, "Files", "tactility.files" },
    { LVGL_ICON_LAUNCHER_SETTINGS, "Settings", "tactility.settings" },
};

void onShortcutPressed(lv_event_t* event) {
    const auto* shortcut = static_cast<const Shortcut*>(lv_event_get_user_data(event));
    AppManifest manifest {};
    if (app_manager_find_manifest(shortcut->appId, &manifest) != ERROR_NONE) {
        return;
    }

    uint32_t instanceId = 0;
    app_start(shortcut->appId, 0, nullptr, &instanceId);
}

void createShortcut(lv_obj_t* grid, const Shortcut* shortcut) {
    auto* button = lv_button_create(grid);
    lv_obj_set_size(button, 94, 82);
    lv_obj_set_style_radius(button, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_theme_get_color_primary(button), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_20, LV_PART_MAIN);
    lv_obj_add_event_cb(button, onShortcutPressed, LV_EVENT_SHORT_CLICKED, const_cast<Shortcut*>(shortcut));

    auto* symbol = lv_label_create(button);
    lv_label_set_text(symbol, shortcut->symbol);
    lv_obj_set_style_text_font(symbol, lvgl_get_launcher_icon_font(), LV_PART_MAIN);
    lv_obj_set_style_text_color(symbol, lv_theme_get_color_primary(symbol), LV_PART_MAIN);
    lv_obj_align(symbol, LV_ALIGN_TOP_MID, 0, 0);

    auto* label = lv_label_create(button);
    lv_label_set_text(label, shortcut->label);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -3);
}

void createWidgets(lv_obj_t* parent, void*) {
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(parent, 8, LV_PART_MAIN);

    auto* header = lv_obj_create(parent);
    lv_obj_set_size(header, LV_PCT(100), 64);
    lv_obj_set_style_radius(header, 14, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(header, lv_theme_get_color_primary(header), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header, LV_OPA_20, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    auto* title = lv_label_create(header);
    lv_label_set_text(title, "Tactility");
    lv_obj_set_style_text_color(title, lv_theme_get_color_primary(title), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    auto* subtitle = lv_label_create(header);
    lv_label_set_text(subtitle, "T-HMI Launcher");
    lv_obj_set_style_text_opa(subtitle, LV_OPA_70, LV_PART_MAIN);
    lv_obj_align(subtitle, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    auto* grid = lv_obj_create(parent);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(grid, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_row(grid, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_column(grid, 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(grid, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, LV_PART_MAIN);

    for (const auto& shortcut : SHORTCUTS) {
        AppManifest manifest {};
        if (app_manager_find_manifest(shortcut.appId, &manifest) == ERROR_NONE) {
            createShortcut(grid, &shortcut);
        }
    }
}

int32_t appMain(int, char**) {
    const uint32_t appInstanceId = app_scheduler_current_app_id();

    TaskEventGroup eventGroup {};
    task_event_group_construct(&eventGroup);

    AppEventSubscription subscription {};
    check(app_event_subscribe(&subscription, &eventGroup) == ERROR_NONE);

    const WindowId window = window_manager_create(appInstanceId, createWidgets, nullptr);

    bool shouldClose = false;
    while (!shouldClose) {
        task_event_group_wait_any(&eventGroup, nullptr, portMAX_DELAY);
        AppEvent event {};
        while (app_event_poll(&subscription, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_CLOSE) {
                shouldClose = true;
                break;
            }
        }
    }

    window_manager_remove(window);
    check(app_event_unsubscribe(&subscription) == ERROR_NONE);
    task_event_group_destruct(&eventGroup);
    return 0;
}

} // namespace

extern const ::AppManifest manifest = {
    .id = "tactility.controlcenter",
    .name = "Control Center",
    .category = APP_CATEGORY_SYSTEM,
    .location = { .type = APP_LOCATION_MEMORY, .location = reinterpret_cast<void*>(appMain) },
    .flags = 0,
    .stack = { .depth = 3072, .desired_memory_capability = MEMORY_CAPABILITY_EXTERNAL }
};

} // namespace tt::app::controlcenter
