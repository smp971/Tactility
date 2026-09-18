// Default headers
#include <tactility/device.h>
#include <tactility/dts.h>
#include <tactility/module.h>
// DTS headers
#include <tactility/bindings/root.h>
#include <tactility/bindings/battery_sense.h>
#include <tactility/bindings/esp32_adc_oneshot.h>
#include <tactility/bindings/esp32_ble.h>
#include <tactility/bindings/esp32_wifi_pinned.h>
#include <tactility/bindings/esp32_gpio.h>
#include <tactility/bindings/esp32_sdmmc.h>
#include <tactility/bindings/esp32_i8080.h>
#include <tactility/bindings/esp32_pwm_ledc.h>
#include <tactility/bindings/esp32_usbdevice.h>
#include <tactility/bindings/pwm_backlight.h>
#include <tactility/bindings/gpio_hog.h>
#include <bindings/button_control.h>
#include <bindings/st7789_i8080.h>
#include <bindings/xpt2046_softspi.h>

static const root_config_dt root_config = {
	"LilyGO T-HMI"
};

static struct Device root = {
	.address = 0,
	.name = "/",
	.config = &root_config,
	.flags = DEVICE_FLAG_DTS,
	.parent = NULL,
	.internal = NULL
};

static const esp32_wifi_pinned_config_dt wifi0_config = {
	0
};

static struct Device wifi0 = {
	.address = 0,
	.name = "wifi0",
	.config = &wifi0_config,
	.flags = DEVICE_FLAG_DTS,
	.parent = &root,
	.internal = NULL
};

static const esp32_ble_config_dt ble0_config = {
	0
};

static struct Device ble0 = {
	.address = 0,
	.name = "ble0",
	.config = &ble0_config,
	.flags = DEVICE_FLAG_DTS,
	.parent = &root,
	.internal = NULL
};

static const esp32_gpio_config_dt gpio0_config = {
	49
};

static struct Device gpio0 = {
	.address = 0,
	.name = "gpio0",
	.config = &gpio0_config,
	.flags = DEVICE_FLAG_DTS,
	.parent = &root,
	.internal = NULL
};

static const gpio_hog_config_dt power_on_config = {
	{ &gpio0,14,GPIO_FLAG_NONE },
	GPIO_HOG_MODE_OUTPUT_HIGH
};

static struct Device power_on = {
	.address = 0,
	.name = "power_on",
	.config = &power_on_config,
	.flags = DEVICE_FLAG_DTS,
	.parent = &root,
	.internal = NULL
};

static const gpio_hog_config_dt power_en_config = {
	{ &gpio0,10,GPIO_FLAG_NONE },
	GPIO_HOG_MODE_OUTPUT_HIGH
};

static struct Device power_en = {
	.address = 0,
	.name = "power_en",
	.config = &power_en_config,
	.flags = DEVICE_FLAG_DTS,
	.parent = &root,
	.internal = NULL
};

static struct Esp32AdcOneshotChannelConfig adc0_channels[] = { { ADC_CHANNEL_4,ADC_ATTEN_DB_12,ADC_BITWIDTH_DEFAULT } };
static const esp32_adc_oneshot_config_dt adc0_config = {
	ADC_UNIT_1,
	0,
	0,
	(struct Esp32AdcOneshotChannelConfig*)adc0_channels,
	1
};

static struct Device adc0 = {
	.address = 0,
	.name = "adc0",
	.config = &adc0_config,
	.flags = DEVICE_FLAG_DTS,
	.parent = &root,
	.internal = NULL
};

static const battery_sense_config_dt battery_sense_config = {
	{ &adc0,0 },
	3300,
	2000
};

static struct Device battery_sense = {
	.address = 0,
	.name = "battery-sense",
	.config = &battery_sense_config,
	.flags = DEVICE_FLAG_DTS,
	.parent = &root,
	.internal = NULL
};

static const esp32_pwm_ledc_config_dt display_backlight_pwm_config = {
	{ &gpio0,38,GPIO_FLAG_NONE },
	33333,
	0,
	false,
	10,
	0,
	0
};

static struct Device display_backlight_pwm = {
	.address = 0,
	.name = "display_backlight_pwm",
	.config = &display_backlight_pwm_config,
	.flags = DEVICE_FLAG_DTS,
	.parent = &root,
	.internal = NULL
};

static const pwm_backlight_config_dt display_backlight_config = {
	&display_backlight_pwm,
	{ 0,255 },
	200
};

static struct Device display_backlight = {
	.address = 0,
	.name = "display_backlight",
	.config = &display_backlight_config,
	.flags = DEVICE_FLAG_DTS,
	.parent = &root,
	.internal = NULL
};

static struct GpioPinSpec i8080_0_cs_gpios[] = { { &gpio0,6,GPIO_FLAG_NONE } };
static const esp32_i8080_config_dt i8080_0_config = {
	{ &gpio0,7,GPIO_FLAG_NONE },
	{ &gpio0,8,GPIO_FLAG_NONE },
	GPIO_PIN_SPEC_NONE,
	{ &gpio0,48,GPIO_FLAG_NONE },
	{ &gpio0,47,GPIO_FLAG_NONE },
	{ &gpio0,39,GPIO_FLAG_NONE },
	{ &gpio0,40,GPIO_FLAG_NONE },
	{ &gpio0,41,GPIO_FLAG_NONE },
	{ &gpio0,42,GPIO_FLAG_NONE },
	{ &gpio0,45,GPIO_FLAG_NONE },
	{ &gpio0,46,GPIO_FLAG_NONE },
	15360,
	(struct GpioPinSpec*)i8080_0_cs_gpios,
	1
};

static struct Device i8080_0 = {
	.address = 0,
	.name = "i8080_0",
	.config = &i8080_0_config,
	.flags = DEVICE_FLAG_DTS,
	.parent = &root,
	.internal = NULL
};

static const st7789_i8080_config_dt display_config = {
	240,
	320,
	0,
	0,
	false,
	false,
	false,
	false,
	false,
	16000000,
	10,
	2,
	GPIO_PIN_SPEC_NONE,
	&display_backlight
};

static struct Device display = {
	.address = 0,
	.name = "display",
	.config = &display_config,
	.flags = DEVICE_FLAG_DTS,
	.parent = &i8080_0,
	.internal = NULL
};

static const xpt2046_softspi_config_dt touch_config = {
	{ &gpio0,3,GPIO_FLAG_NONE },
	{ &gpio0,4,GPIO_FLAG_NONE },
	{ &gpio0,1,GPIO_FLAG_NONE },
	{ &gpio0,2,GPIO_FLAG_NONE },
	240,
	320,
	false,
	false,
	false,
	false,
	4200
};

static struct Device touch = {
	.address = 0,
	.name = "touch",
	.config = &touch_config,
	.flags = DEVICE_FLAG_DTS,
	.parent = &root,
	.internal = NULL
};

static const esp32_sdmmc_config_dt sdmmc0_config = {
	{ &gpio0,12,GPIO_FLAG_NONE },
	{ &gpio0,11,GPIO_FLAG_NONE },
	{ &gpio0,13,GPIO_FLAG_NONE },
	GPIO_PIN_SPEC_NONE,
	GPIO_PIN_SPEC_NONE,
	GPIO_PIN_SPEC_NONE,
	GPIO_PIN_SPEC_NONE,
	GPIO_PIN_SPEC_NONE,
	GPIO_PIN_SPEC_NONE,
	GPIO_PIN_SPEC_NONE,
	GPIO_PIN_SPEC_NONE,
	GPIO_PIN_SPEC_NONE,
	1,
	SDMMC_HOST_SLOT_1,
	20000,
	false,
	false,
	true,
	-1
};

static struct Device sdmmc0 = {
	.address = 0,
	.name = "sdmmc0",
	.config = &sdmmc0_config,
	.flags = DEVICE_FLAG_DTS,
	.parent = &root,
	.internal = NULL
};

static const esp32_usbdevice_config_dt usbdevice0_config = {
	0
};

static struct Device usbdevice0 = {
	.address = 0,
	.name = "usbdevice0",
	.config = &usbdevice0_config,
	.flags = DEVICE_FLAG_DTS,
	.parent = &root,
	.internal = NULL
};

static const esp32_usbdevice_msc_config_dt usbdevicemsc0_config = {
	0
};

static struct Device usbdevicemsc0 = {
	.address = 0,
	.name = "usbdevicemsc0",
	.config = &usbdevicemsc0_config,
	.flags = DEVICE_FLAG_DTS,
	.parent = &usbdevice0,
	.internal = NULL
};

const struct DtsDevice dts_devices[] = {
	{ &root, "root", DTS_DEVICE_STATUS_OKAY },
	{ &wifi0, "espressif,esp32-wifi-pinned", DTS_DEVICE_STATUS_OKAY },
	{ &ble0, "espressif,esp32-ble", DTS_DEVICE_STATUS_OKAY },
	{ &gpio0, "espressif,esp32-gpio", DTS_DEVICE_STATUS_OKAY },
	{ &power_on, "gpio-hog", DTS_DEVICE_STATUS_OKAY },
	{ &power_en, "gpio-hog", DTS_DEVICE_STATUS_OKAY },
	{ &adc0, "espressif,esp32-adc-oneshot", DTS_DEVICE_STATUS_OKAY },
	{ &battery_sense, "battery-sense", DTS_DEVICE_STATUS_OKAY },
	{ &display_backlight_pwm, "espressif,esp32-pwm-ledc", DTS_DEVICE_STATUS_OKAY },
	{ &display_backlight, "pwm-backlight", DTS_DEVICE_STATUS_DISABLED },
	{ &i8080_0, "espressif,esp32-i8080", DTS_DEVICE_STATUS_OKAY },
	{ &display, "sitronix,st7789-i8080", DTS_DEVICE_STATUS_OKAY },
	{ &touch, "xptek,xpt2046-softspi", DTS_DEVICE_STATUS_OKAY },
	{ &sdmmc0, "espressif,esp32-sdmmc", DTS_DEVICE_STATUS_DISABLED },
	{ &usbdevice0, "espressif,esp32-usbdevice", DTS_DEVICE_STATUS_OKAY },
	{ &usbdevicemsc0, "espressif,esp32-usbdevice-msc", DTS_DEVICE_STATUS_OKAY },
	DTS_DEVICE_TERMINATOR
};

extern struct Module lilygo_thmi_module;
extern struct Module platform_esp32_module;
extern struct Module xpt2046_softspi_module;
extern struct Module st7789_i8080_module;
extern struct Module button_control_module;

struct Module* const dts_modules[] = {
	&lilygo_thmi_module,
	&platform_esp32_module,
	&xpt2046_softspi_module,
	&st7789_i8080_module,
	&button_control_module,
	NULL
};
