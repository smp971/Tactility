#include <app/event.h>
#include <app/manifest.h>
#include <app/scheduler.h>
#include <lvgl/lvgl.h>
#include <lvgl/fonts.h>
#include <lvgl_window_manager/window_manager.h>
#include <lvgl/widgets/toolbar.h>
#include <tactility/check.h>
#include <tactility/device.h>
#include <tactility/drivers/bluetooth_hid_device.h>
#include <tactility/memory.h>
#include <Tactility/Tactility.h>
#ifdef ESP_PLATFORM
#include <cJSON.h>
#endif
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace tt::app::btremote { namespace {
constexpr auto* CONFIG_PATH = "/data/bluetooth_remote.json";
constexpr size_t BUTTON_COUNT = 12, PROFILE_COUNT = 3;
enum class ActionType { Consumer, Keyboard };
struct Action { ActionType type = ActionType::Consumer; uint16_t code = 0; uint8_t modifier = 0; };
struct ButtonConfig { std::string label, icon; uint32_t color = 0x2D3035; Action shortAction, longAction; };
struct Profile { std::string name; std::array<ButtonConfig, BUTTON_COUNT> buttons; };
using Profiles = std::array<Profile, PROFILE_COUNT>;
Action consumer(uint16_t code) { return {ActionType::Consumer, code, 0}; }
Action key(uint8_t code, uint8_t modifier = 0) { return {ActionType::Keyboard, code, modifier}; }
ButtonConfig b(const char* label, const char* icon, Action shortAction, Action longAction = {}) { return {label, icon, 0x2D3035, shortAction, longAction}; }

Profiles defaults() { return {{
 {"Media", {{b("Anterior","prev",consumer(182)),b("Play / Pauza","play",consumer(205)),b("Urmator","next",consumer(181)),b("Volum -","vol_down",consumer(234),consumer(234)),b("Mute","mute",consumer(226)),b("Volum +","vol_up",consumer(233),consumer(233)),b("Stop","stop",consumer(183)),b("Home","home",consumer(547)),b("Back","back",consumer(548)),b("Calculator","calc",consumer(402)),b("Email","mail",consumer(394)),b("Browser","browser",consumer(406))}}},
 {"Office", {{b("Copy","copy",key(0x06,1)),b("Paste","paste",key(0x19,1)),b("Cut","cut",key(0x1B,1)),b("Undo","undo",key(0x1D,1)),b("Redo","redo",key(0x1C,1)),b("Save","save",key(0x16,1)),b("Select all","select",key(0x04,1)),b("Find","search",key(0x09,1)),b("Print","print",key(0x13,1)),b("Alt + Tab","switch",key(0x2B,4)),b("Enter","enter",key(0x28)),b("Delete","delete",key(0x4C))}}},
 {"Browser", {{b("Tab nou","plus",key(0x17,1)),b("Redeschide","undo",key(0x17,3)),b("Inchide tab","close",key(0x1A,1)),b("Tab stanga","prev",key(0x2B,3)),b("Tab dreapta","next",key(0x2B,5)),b("Refresh","refresh",key(0x15,1)),b("Adresa","search",key(0x0F,1)),b("Back","back",key(0x50,4)),b("Forward","next",key(0x4F,4)),b("Sus","up",key(0x4B)),b("Jos","down",key(0x4E)),b("Full screen","screen",key(0x3D))}}}
}}; }

#ifdef ESP_PLATFORM
std::string js(cJSON* o,const char* n,const std::string& f={}) { auto*i=cJSON_GetObjectItemCaseSensitive(o,n); return cJSON_IsString(i)&&i->valuestring?i->valuestring:f; }
int ji(cJSON* o,const char* n,int f=0) { auto*i=cJSON_GetObjectItemCaseSensitive(o,n); return cJSON_IsNumber(i)?i->valueint:f; }
Action readAction(cJSON* o,const Action& f) { if(!cJSON_IsObject(o))return f; Action a=f; a.type=js(o,"type")=="keyboard"?ActionType::Keyboard:ActionType::Consumer; a.code=ji(o,"code",ji(o,"usage",a.code)); a.modifier=ji(o,"modifier",a.modifier); return a; }
#endif
Profiles loadProfiles() { auto ps=defaults();
#ifdef ESP_PLATFORM
 FILE*f=fopen(CONFIG_PATH,"rb"); if(!f)return ps; std::string j; char buf[256]; while(size_t n=fread(buf,1,sizeof(buf),f))j.append(buf,n); fclose(f); auto*r=cJSON_Parse(j.c_str()); auto*pl=r?cJSON_GetObjectItemCaseSensitive(r,"profiles"):nullptr;
 if(cJSON_IsArray(pl)) for(size_t p=0;p<PROFILE_COUNT;p++){auto*po=cJSON_GetArrayItem(pl,p);if(!cJSON_IsObject(po))continue;ps[p].name=js(po,"name",ps[p].name);auto*ba=cJSON_GetObjectItemCaseSensitive(po,"buttons");for(size_t i=0;cJSON_IsArray(ba)&&i<BUTTON_COUNT;i++){auto*o=cJSON_GetArrayItem(ba,i);auto&t=ps[p].buttons[i];t.label=js(o,"label",t.label);t.icon=js(o,"icon",t.icon);auto c=js(o,"color");if(!c.empty())t.color=strtoul(c.c_str()+(c[0]=='#'),nullptr,16);t.shortAction=readAction(cJSON_GetObjectItemCaseSensitive(o,"short"),t.shortAction);t.longAction=readAction(cJSON_GetObjectItemCaseSensitive(o,"long"),t.longAction);}}
 else {auto*ba=r?cJSON_GetObjectItemCaseSensitive(r,"buttons"):nullptr;for(size_t i=0;cJSON_IsArray(ba)&&i<BUTTON_COUNT;i++){auto*o=cJSON_GetArrayItem(ba,i);ps[0].buttons[i].label=js(o,"label",ps[0].buttons[i].label);ps[0].buttons[i].shortAction.code=ji(o,"usage",ps[0].buttons[i].shortAction.code);}} cJSON_Delete(r);
#endif
 return ps; }

struct Context { uint32_t appInstanceId=0; Device*hid=nullptr; lv_obj_t*status=nullptr,*profileLabel=nullptr,*grid=nullptr; Profiles profiles=defaults(); size_t currentProfile=0; };
const char* iconText(const std::string&i){if(i=="prev"||i=="back")return LV_SYMBOL_PREV;if(i=="next"||i=="forward")return LV_SYMBOL_NEXT;if(i=="play")return LV_SYMBOL_PLAY;if(i=="stop")return LV_SYMBOL_STOP;if(i=="mute")return LV_SYMBOL_MUTE;if(i=="vol_down")return LV_SYMBOL_VOLUME_MID;if(i=="vol_up")return LV_SYMBOL_VOLUME_MAX;if(i=="home")return LV_SYMBOL_HOME;if(i=="plus")return LV_SYMBOL_PLUS;if(i=="close")return LV_SYMBOL_CLOSE;if(i=="refresh"||i=="redo")return LV_SYMBOL_REFRESH;if(i=="search"||i=="find")return LV_SYMBOL_EYE_OPEN;if(i=="up")return LV_SYMBOL_UP;if(i=="down")return LV_SYMBOL_DOWN;if(i=="mail")return LV_SYMBOL_ENVELOPE;if(i=="save")return LV_SYMBOL_SAVE;return LV_SYMBOL_SETTINGS;}
void sendAction(Context*c,const Action&a){if(!a.code){lv_label_set_text(c->status,"Nicio actiune configurata");return;}if(!c->hid||!bluetooth_hid_device_is_connected(c->hid)){lv_label_set_text(c->status,"Neconectat - asociaza Tactility Remote");return;}if(a.type==ActionType::Consumer){uint8_t r[2]{uint8_t(a.code),uint8_t(a.code>>8)},z[2]{};bluetooth_hid_device_send_consumer(c->hid,r,2);bluetooth_hid_device_send_consumer(c->hid,z,2);}else{uint8_t r[8]{a.modifier,0,uint8_t(a.code),0,0,0,0,0},z[8]{};bluetooth_hid_device_send_keyboard(c->hid,r,8);bluetooth_hid_device_send_keyboard(c->hid,z,8);}lv_label_set_text(c->status,"Comanda trimisa");}
void onButton(lv_event_t*e){auto*c=static_cast<Context*>(lv_event_get_user_data(e));size_t i=reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(e)));auto&b=c->profiles[c->currentProfile].buttons[i];sendAction(c,lv_event_get_code(e)==LV_EVENT_LONG_PRESSED?b.longAction:b.shortAction);}
void drawGrid(Context*c){lv_obj_clean(c->grid);lv_label_set_text(c->profileLabel,c->profiles[c->currentProfile].name.c_str());for(size_t i=0;i<BUTTON_COUNT;i++){auto&it=c->profiles[c->currentProfile].buttons[i];auto*bt=lv_button_create(c->grid);lv_obj_set_size(bt,LV_PCT(30),LV_PCT(21));lv_obj_set_style_bg_color(bt,lv_color_hex(it.color),0);lv_obj_set_style_bg_color(bt,lv_color_hex(0x3B82F6),LV_STATE_PRESSED);lv_obj_set_style_radius(bt,7,0);lv_obj_set_style_shadow_width(bt,0,0);lv_obj_set_style_pad_all(bt,3,0);lv_obj_set_flex_flow(bt,LV_FLEX_FLOW_COLUMN);lv_obj_set_flex_align(bt,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);lv_obj_set_user_data(bt,reinterpret_cast<void*>(i));auto*ic=lv_label_create(bt);lv_label_set_text(ic,iconText(it.icon));auto*l=lv_label_create(bt);lv_label_set_text(l,it.label.c_str());lv_obj_set_width(l,LV_PCT(94));lv_obj_set_style_text_font(l,lvgl_get_text_font(FONT_SIZE_SMALL),0);lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);lv_label_set_long_mode(l,LV_LABEL_LONG_WRAP);lv_obj_add_event_cb(bt,onButton,LV_EVENT_SHORT_CLICKED,c);lv_obj_add_event_cb(bt,onButton,LV_EVENT_LONG_PRESSED,c);}}
void onProfile(lv_event_t*e){auto*c=static_cast<Context*>(lv_event_get_user_data(e));c->currentProfile=reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(e)));drawGrid(c);}
void onBack(lv_event_t*e){app_event_emit_close(static_cast<Context*>(lv_event_get_user_data(e))->appInstanceId);}
void createWidgets(lv_obj_t*p,void*d){auto*c=static_cast<Context*>(d);lv_obj_set_flex_flow(p,LV_FLEX_FLOW_COLUMN);lv_obj_set_style_pad_row(p,0,0);auto*t=lvgl_toolbar_create(p,"Bluetooth Remote v2");lvgl_toolbar_set_nav_action(t,LV_SYMBOL_CLOSE,onBack,c);auto*co=lv_obj_create(p);lv_obj_set_width(co,LV_PCT(100));lv_obj_set_flex_grow(co,1);lv_obj_set_style_border_width(co,0,0);lv_obj_set_style_pad_all(co,6,0);lv_obj_set_style_bg_color(co,lv_color_hex(0x181A1D),0);lv_obj_set_style_text_color(co,lv_color_white(),0);lv_obj_set_flex_flow(co,LV_FLEX_FLOW_COLUMN);auto*n=lv_obj_create(co);lv_obj_set_width(n,LV_PCT(100));lv_obj_set_height(n,30);lv_obj_set_style_border_width(n,0,0);lv_obj_set_style_pad_all(n,0,0);lv_obj_set_flex_flow(n,LV_FLEX_FLOW_ROW);for(size_t i=0;i<PROFILE_COUNT;i++){auto*bt=lv_button_create(n);lv_obj_set_flex_grow(bt,1);lv_obj_set_height(bt,28);lv_obj_set_user_data(bt,reinterpret_cast<void*>(i));auto*l=lv_label_create(bt);lv_label_set_text(l,c->profiles[i].name.c_str());lv_obj_center(l);lv_obj_add_event_cb(bt,onProfile,LV_EVENT_SHORT_CLICKED,c);}c->profileLabel=lv_label_create(co);lv_obj_set_style_text_font(c->profileLabel,lvgl_get_text_font(FONT_SIZE_SMALL),0);c->status=lv_label_create(co);lv_label_set_text(c->status,c->hid&&bluetooth_hid_device_is_connected(c->hid)?"Conectat":"Cauta Tactility Remote");c->grid=lv_obj_create(co);lv_obj_set_width(c->grid,LV_PCT(100));lv_obj_set_flex_grow(c->grid,1);lv_obj_set_style_border_width(c->grid,0,0);lv_obj_set_style_pad_all(c->grid,2,0);lv_obj_set_flex_flow(c->grid,LV_FLEX_FLOW_ROW_WRAP);lv_obj_set_flex_align(c->grid,LV_FLEX_ALIGN_SPACE_EVENLY,LV_FLEX_ALIGN_SPACE_EVENLY,LV_FLEX_ALIGN_CENTER);drawGrid(c);}
int32_t appMain(int,char**){auto*c=new Context();c->appInstanceId=app_scheduler_current_app_id();c->profiles=loadProfiles();c->hid=bluetooth_hid_device_get();if(c->hid)bluetooth_hid_device_start(c->hid,BT_HID_DEVICE_MODE_KEYBOARD);TaskEventGroup eg{};task_event_group_construct(&eg);AppEventSubscription s{};check(app_event_subscribe(&s,&eg)==ERROR_NONE);WindowId w=window_manager_create(c->appInstanceId,createWidgets,c);bool close=false;while(!close){task_event_group_wait_any(&eg,nullptr,portMAX_DELAY);AppEvent e{};while(app_event_poll(&s,&e)==ERROR_NONE)if(e.type==APP_EVENT_CLOSE)close=true;}window_manager_remove(w);if(c->hid)device_put(c->hid);check(app_event_unsubscribe(&s)==ERROR_NONE);task_event_group_destruct(&eg);delete c;return 0;}
}
extern const ::AppManifest manifest={.id="tactility.btremote",.name="Bluetooth Remote",.category=APP_CATEGORY_USER,.location={APP_LOCATION_MEMORY,reinterpret_cast<void*>(appMain)},.flags=0,.stack={.depth=8192,.desired_memory_capability=MEMORY_CAPABILITY_EXTERNAL}};
}
