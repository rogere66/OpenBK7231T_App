
#include "../logging/logging.h"
#include "../new_pins.h"
#include "../new_cfg.h"
#include "cmd_public.h"
#include "../obk_config.h"
#include "../driver/drv_public.h"
#include "../hal/hal_flashVars.h"
#include "../rgb2hsv.h"
#include <ctype.h>
#include "cmd_local.h"
#include "../mqtt/new_mqtt.h"
#include "../cJSON/cJSON.h"
#ifdef BK_LITTLEFS
	#include "../littlefs/our_lfs.h"
#endif

//  My HA config for system below:
/*
  - platform: mqtt
    name: obk8D38570E
    rgb_command_template: "{{ '#%02x%02x%02x0000' | format(red, green, blue)}}"
    rgb_state_topic: "cmnd/obk8D38570E/led_basecolor_rgb"
    rgb_command_topic: "cmnd/obk8D38570E/led_basecolor_rgb"
    command_topic: "cmnd/obk8D38570E/led_enableAll"
    availability_topic: "obk8D38570E/connected"
    payload_on: 1
    payload_off: 0
    brightness_command_topic: "cmnd/obk8D38570E/led_dimmer"
    brightness_scale: 100
    brightness_value_template: "{{ value_json.Dimmer }}"
    color_temp_command_topic: "cmnd/obk8D38570E/led_temperature"
    color_temp_state_topic: "cmnd/obk8D38570E/ctr"
    color_temp_value_template: "{{ value_json.CT }}"

	// TODO: state return topics
	*/

// NOTE: there are 2 customization commands
// They are not storing the config in flash, if you use them,
// please put them in autoexec.bat from LittleFS.
// Command 1: led_brightnessMult [floatVal]
// It sets the multipler for the dimming
// Command 2: g_cfg_colorScaleToChannel [floatVal]
// It sets the multipler for converting 0-255 range RGB to 0-100 channel value

int parsePowerArgument(const char *s);


int g_lightMode = Light_RGB;
// Those are base colors, normalized, without brightness applied
float baseColors[5] = { 255, 255, 255, 255, 255 };
// Those have brightness included
float finalColors[5] = { 255, 255, 255, 255, 255 };
float g_hsv_h = 0; // 0 to 360
float g_hsv_s = 0; // 0 to 1
float g_hsv_v = 1; // 0 to 1
// By default, colors are in 255 to 0 range, while our channels accept 0 to 100 range
float g_cfg_colorScaleToChannel = 100.0f/255.0f;
int g_numBaseColors = 5;
float g_brightness = 1.0f;

// NOTE: in this system, enabling/disabling whole led light bulb
// is not changing the stored channel and brightness values.
// They are kept intact so you can reenable the bulb and keep your color setting
int g_lightEnableAll = 0;
// config only stuff
float g_cfg_brightnessMult = 0.01f;

// the slider control in the UI emits values
//in the range from 154-500 (defined
//in homeassistant/util/color.py as HASS_COLOR_MIN and HASS_COLOR_MAX).
float led_temperature_min = HASS_TEMPERATURE_MIN;
float led_temperature_max = HASS_TEMPERATURE_MAX;
float led_temperature_current = HASS_TEMPERATURE_MIN;

void LED_ResetGlobalVariablesToDefaults() {
	int i;

	g_lightMode = Light_RGB;
	for (i = 0; i < 5; i++) {
		baseColors[i] = 255;
		finalColors[i] = 255;
	}
	g_hsv_h = 0; // 0 to 360
	g_hsv_s = 0; // 0 to 1
	g_hsv_v = 1; // 0 to 1
	g_cfg_colorScaleToChannel = 100.0f / 255.0f;
	g_numBaseColors = 5;
	g_brightness = 1.0f;
	g_lightEnableAll = 0;
	g_cfg_brightnessMult = 0.01f;
	led_temperature_min = HASS_TEMPERATURE_MIN;
	led_temperature_max = HASS_TEMPERATURE_MAX;
	led_temperature_current = HASS_TEMPERATURE_MIN;
}

bool LED_IsLedDriverChipRunning()
{
#ifndef OBK_DISABLE_ALL_DRIVERS
	return DRV_IsRunning("SM2135") || DRV_IsRunning("BP5758D") || DRV_IsRunning("TESTLED");
#else
	return false;
#endif
}
bool LED_IsLEDRunning()
{
	int pwmCount;

	if (LED_IsLedDriverChipRunning())
		return true;

	pwmCount = PIN_CountPinsWithRoleOrRole(IOR_PWM, IOR_PWM_n);
	if (pwmCount > 0)
		return true;
	return false;
}

int isCWMode() {
	int pwmCount;

	pwmCount = PIN_CountPinsWithRoleOrRole(IOR_PWM, IOR_PWM_n);

	if(pwmCount == 2)
		return 1;
	return 0;
}

int shouldSendRGB() {
	int pwmCount;

	// forced RGBCW means 'send rgb'
	// This flag should be set for SM2315 and BP5758
	// This flag also could be used for dummy Device Groups driver-module
	if(CFG_HasFlag(OBK_FLAG_LED_FORCESHOWRGBCWCONTROLLER))
		return 1;

	pwmCount = PIN_CountPinsWithRoleOrRole(IOR_PWM, IOR_PWM_n);

	// single colors and CW don't send rgb
	if(pwmCount <= 2)
		return 0;

	return 1;
}


// One user requested ability to broadcast full RGBCW
static void sendFullRGBCW_IfEnabled() {
	char s[16];
	byte c[5];

	if(CFG_HasFlag(OBK_FLAG_LED_BROADCAST_FULL_RGBCW) == false) {
		return;
	}

	c[0] = (byte)(finalColors[0]);
	c[1] = (byte)(finalColors[1]);
	c[2] = (byte)(finalColors[2]);
	c[3] = (byte)(finalColors[3]);
	c[4] = (byte)(finalColors[4]);

	snprintf(s, sizeof(s),"%02X%02X%02X%02X%02X",c[0],c[1],c[2],c[3],c[4]);

	MQTT_PublishMain_StringString_DeDuped(DEDUP_LED_FINALCOLOR_RGBCW,DEDUP_EXPIRE_TIME,"led_finalcolor_rgbcw",s, 0);
}

float led_rawLerpCurrent[5] = { 0 };
float Mathf_MoveTowards(float cur, float tg, float dt) {
	float rem = tg - cur;
	if(abs(rem) < dt) {
		return tg;
	}
	if(rem < 0) {
		return cur - dt;
	}
	return cur + dt;
}
// Colors are in 0-255 range.
// This value determines how fast color can change.
// 100 means that in one second color will go from 0 to 100
// 200 means that in one second color will go from 0 to 200
float led_lerpSpeedUnitsPerSecond = 200.f;

float led_current_value_brightness = 0;
float led_current_value_cold_or_warm = 0;

void LED_RunQuickColorLerp(int deltaMS) {
	int i;
	int firstChannelIndex;
	float deltaSeconds;
	byte finalRGBCW[5];
	int maxPossibleIndexToSet;

	if (CFG_HasFlag(OBK_FLAG_LED_FORCE_MODE_RGB)) {
		// only allow setting pwm 0, 1 and 2, force-skip 3 and 4
		maxPossibleIndexToSet = 3;
	}
	else {
		maxPossibleIndexToSet = 5;
	}

	deltaSeconds = deltaMS * 0.001f;

	// The color order is RGBCW.
	// some people set RED to channel 0, and some of them set RED to channel 1
	// Let's detect if there is a PWM on channel 0
	if(CHANNEL_HasChannelPinWithRoleOrRole(0, IOR_PWM, IOR_PWM_n)) {
		firstChannelIndex = 0;
	} else {
		firstChannelIndex = 1;
	}

	for(i = 0; i < 5; i++) {
		// This is the most silly and primitive approach, but it works
		// In future we might implement better lerp algorithms, use HUE, etc
		led_rawLerpCurrent[i] = Mathf_MoveTowards(led_rawLerpCurrent[i],finalColors[i], deltaSeconds * led_lerpSpeedUnitsPerSecond);
	}

	if(isCWMode() && CFG_HasFlag(OBK_FLAG_LED_ALTERNATE_CW_MODE)) {
		// OBK_FLAG_LED_ALTERNATE_CW_MODE means we have a driver that takes one PWM for brightness and second for temperature
		int target_value_brightness = 0;
		int target_value_cold_or_warm = 0;

		target_value_cold_or_warm = LED_GetTemperature0to1Range() * 100.0f;
		target_value_brightness = g_brightness * 100.0f;

		led_current_value_brightness = Mathf_MoveTowards(led_current_value_brightness, target_value_brightness, deltaSeconds * led_lerpSpeedUnitsPerSecond);
		led_current_value_cold_or_warm = Mathf_MoveTowards(led_current_value_cold_or_warm, target_value_cold_or_warm, deltaSeconds * led_lerpSpeedUnitsPerSecond);

		CHANNEL_Set(firstChannelIndex, led_current_value_cold_or_warm, CHANNEL_SET_FLAG_SKIP_MQTT | CHANNEL_SET_FLAG_SILENT);
		CHANNEL_Set(firstChannelIndex+1, led_current_value_brightness, CHANNEL_SET_FLAG_SKIP_MQTT | CHANNEL_SET_FLAG_SILENT);
	} else {
		if(isCWMode()) { 
			// In CW mode, user sets just two PWMs. So we have: PWM0 and PWM1 (or maybe PWM1 and PWM2)
			// But we still have RGBCW internally
			// So, we need to map. Map component 3 of RGBCW to first channel, and component 4 to second.
			CHANNEL_Set(firstChannelIndex + 0, led_rawLerpCurrent[3] * g_cfg_colorScaleToChannel, CHANNEL_SET_FLAG_SKIP_MQTT | CHANNEL_SET_FLAG_SILENT);
			CHANNEL_Set(firstChannelIndex + 1, led_rawLerpCurrent[4] * g_cfg_colorScaleToChannel, CHANNEL_SET_FLAG_SKIP_MQTT | CHANNEL_SET_FLAG_SILENT);
		} else {
			// This should work for both RGB and RGBCW
			// This also could work for a SINGLE COLOR strips
			for(i = 0; i < maxPossibleIndexToSet; i++) {
				finalRGBCW[i] = led_rawLerpCurrent[i];
				CHANNEL_Set(firstChannelIndex + i, led_rawLerpCurrent[i] * g_cfg_colorScaleToChannel, CHANNEL_SET_FLAG_SKIP_MQTT | CHANNEL_SET_FLAG_SILENT);
			}
		}
	}
#ifndef OBK_DISABLE_ALL_DRIVERS
	if(DRV_IsRunning("SM2135")) {
		SM2135_Write(finalRGBCW);
	}
	if(DRV_IsRunning("BP5758D")) {
		BP5758D_Write(finalRGBCW);
	}
	if(DRV_IsRunning("BP1658CJ")) {
		BP1658CJ_Write(finalRGBCW);
	}
#endif
}

int exponential_mode = 2;

/* exponential mode command handler, usage: led_expoMode <0|1|2|3|4>
   exponential modes: 0 = Off
                      1 = 1% min brightness with moderate exponential
                      2 = 1% min brightness with full exponential
                      3 = 0.1% min brightness with moderate exponential
                      4 = 0.1% min brightness with full exponential
*/
static commandResult_t exponentialMode (const void *context, const char *cmd, const char *args, int cmdFlags) {
	int mode = atoi (args);
	if((mode >= 0) && (mode <= 4)) {
		exponential_mode = mode;
	}
	return CMD_RES_OK;
}

void apply_smart_light() {
	int i;
	int firstChannelIndex;
	int channelToUse;
	byte finalRGBCW[5];
	int maxPossibleIndexToSet;

	// The color order is RGBCW.
	// some people set RED to channel 0, and some of them set RED to channel 1
	// Let's detect if there is a PWM on channel 0
	if(CHANNEL_HasChannelPinWithRoleOrRole(0, IOR_PWM, IOR_PWM_n)) {
		firstChannelIndex = 0;
	} else {
		firstChannelIndex = 1;
	}

	if (CFG_HasFlag(OBK_FLAG_LED_FORCE_MODE_RGB)) {
		// only allow setting pwm 0, 1 and 2, force-skip 3 and 4
		maxPossibleIndexToSet = 3;
	}
	else {
		maxPossibleIndexToSet = 5;
	}


	if(isCWMode() && CFG_HasFlag(OBK_FLAG_LED_ALTERNATE_CW_MODE)) {
		int value_brightness = 0;
		int value_cold_or_warm = 0;

		for(i = 0; i < 5; i++) {
			finalColors[i] = 0;
			finalRGBCW[i] = 0;
		}
		if(g_lightEnableAll) {
			value_cold_or_warm = LED_GetTemperature0to1Range() * 100.0f;
			value_brightness = g_brightness * 100.0f;
			for(i = 3; i < 5; i++) {
				finalColors[i] = baseColors[i] * g_brightness;
				finalRGBCW[i] = baseColors[i] * g_brightness;
			}
		}
		if(CFG_HasFlag(OBK_FLAG_LED_SMOOTH_TRANSITIONS) == false) {
			CHANNEL_Set(firstChannelIndex, value_cold_or_warm, CHANNEL_SET_FLAG_SKIP_MQTT | CHANNEL_SET_FLAG_SILENT);
			CHANNEL_Set(firstChannelIndex+1, value_brightness, CHANNEL_SET_FLAG_SKIP_MQTT | CHANNEL_SET_FLAG_SILENT);
		}
	} else {
		for(i = 0; i < maxPossibleIndexToSet; i++) {
			float raw, final;

			raw = baseColors[i];
			final = 0.0f;

			if(g_lightEnableAll) {
				// make brightness exponential:
				if (exponential_mode == 0) {
					final = raw * g_brightness;
				} else if (g_brightness == 0.0f) {
					final = 0.0f;
				} else {
					float expo_base;
					float expo_factor;
					float expo_offset = 0.0;
					if ((exponential_mode == 1) || (exponential_mode == 2)) {
						expo_offset = 0.009f;
					}
					if ((exponential_mode == 1) || (exponential_mode == 3)) {
						expo_base    =  1.2f;     // moderate exponential
						expo_factor  = 15.32f;
						expo_offset -=  0.06609f;
					} else {
						expo_base    =  1.06f;    // full exponential
						expo_factor  = 74.115f;
						expo_offset -=  0.013f;
					}
					final = raw * (pow(expo_base, g_brightness * expo_factor) / expo_factor + expo_offset);
				}
			}
			if(g_lightMode == Light_Temperature) {
				// skip channels 0, 1, 2
				// (RGB)
				if(i < 3)
				{
					final = 0;
				}
			} else if(g_lightMode == Light_RGB) {
				// skip channels 3, 4
				if(i >= 3)
				{
					final = 0;
				}
			} else {

			}
			finalColors[i] = final;
			finalRGBCW[i] = final;

			final *= g_cfg_colorScaleToChannel;
			if (final > 100.0f)
				final = 100.0f;

			channelToUse = firstChannelIndex + i;

			// log printf with %f crashes N platform?
			//ADDLOG_INFO(LOG_FEATURE_CMD, "apply_smart_light: ch %i raw is %f, bright %f, final %f, enableAll is %i",
			//	channelToUse,raw,g_brightness,final,g_lightEnableAll);

			if(CFG_HasFlag(OBK_FLAG_LED_SMOOTH_TRANSITIONS) == false) {
				if(isCWMode()) {
					// in CW mode, we have only set two channels
					// We don't have RGB channels
					// so, do simple mapping
					if(i == 3) {
						CHANNEL_Set(firstChannelIndex+0, final, CHANNEL_SET_FLAG_SKIP_MQTT | CHANNEL_SET_FLAG_SILENT);
					} else if(i == 4) {
						CHANNEL_Set(firstChannelIndex+1, final, CHANNEL_SET_FLAG_SKIP_MQTT | CHANNEL_SET_FLAG_SILENT);
					}
				} else {
					CHANNEL_Set(channelToUse, final, CHANNEL_SET_FLAG_SKIP_MQTT | CHANNEL_SET_FLAG_SILENT);
				}
			}
		}
	}
	if(CFG_HasFlag(OBK_FLAG_LED_SMOOTH_TRANSITIONS) == false) {
#ifndef OBK_DISABLE_ALL_DRIVERS
		if(DRV_IsRunning("SM2135")) {
			SM2135_Write(finalRGBCW);
		}
		if(DRV_IsRunning("BP5758D")) {
			BP5758D_Write(finalRGBCW);
		}
		if(DRV_IsRunning("BP1658CJ")) {
			BP1658CJ_Write(finalRGBCW);
		}
#endif
	}

	if(CFG_HasFlag(OBK_FLAG_LED_REMEMBERLASTSTATE)) {
		HAL_FlashVars_SaveLED(g_lightMode,g_brightness / g_cfg_brightnessMult, led_temperature_current,baseColors[0],baseColors[1],baseColors[2],g_lightEnableAll);
	}
#ifndef OBK_DISABLE_ALL_DRIVERS
	DRV_DGR_OnLedFinalColorsChange(finalRGBCW);
#endif

	// I am not sure if it's the best place to do it
	// NOTE: this will broadcast MQTT only if a flag is set
	sendFullRGBCW_IfEnabled();
}


static OBK_Publish_Result sendColorChange() {
	char s[16];
	byte c[3];

	if(shouldSendRGB()==0) {
		return OBK_PUBLISH_WAS_NOT_REQUIRED;
	}

	c[0] = (byte)(baseColors[0]);
	c[1] = (byte)(baseColors[1]);
	c[2] = (byte)(baseColors[2]);

	snprintf(s, sizeof(s), "%02X%02X%02X",c[0],c[1],c[2]);

	return MQTT_PublishMain_StringString_DeDuped(DEDUP_LED_BASECOLOR_RGB,DEDUP_EXPIRE_TIME,"led_basecolor_rgb",s, 0);
}
void LED_GetBaseColorString(char * s) {
	byte c[3];

	c[0] = (byte)(baseColors[0]);
	c[1] = (byte)(baseColors[1]);
	c[2] = (byte)(baseColors[2]);

	sprintf(s, "%02X%02X%02X",c[0],c[1],c[2]);
}
static void sendFinalColor() {
	char s[16];
	byte c[3];

	if(shouldSendRGB()==0) {
		return;
	}

	c[0] = (byte)(finalColors[0]);
	c[1] = (byte)(finalColors[1]);
	c[2] = (byte)(finalColors[2]);

	snprintf(s, sizeof(s),"%02X%02X%02X",c[0],c[1],c[2]);

	MQTT_PublishMain_StringString_DeDuped(DEDUP_LED_FINALCOLOR_RGB,DEDUP_EXPIRE_TIME,"led_finalcolor_rgb",s, 0);
}
OBK_Publish_Result LED_SendDimmerChange() {
	int iValue;

	iValue = g_brightness / g_cfg_brightnessMult;

	return MQTT_PublishMain_StringInt_DeDuped(DEDUP_LED_DIMMER,DEDUP_EXPIRE_TIME,"led_dimmer", iValue, 0);
}
static OBK_Publish_Result sendTemperatureChange(){
	return MQTT_PublishMain_StringInt_DeDuped(DEDUP_LED_TEMPERATURE,DEDUP_EXPIRE_TIME,"led_temperature", (int)led_temperature_current,0);
}
float LED_GetTemperature() {
	return led_temperature_current;
}
int LED_GetMode() {
	return g_lightMode;
}

const char *GetLightModeStr(int mode) {
	if(mode == Light_All)
		return "all";
	if(mode == Light_Temperature)
		return "cw";
	if(mode == Light_RGB)
		return "rgb";
	return "er";
}
void SET_LightMode(int newMode) {
	if(g_lightMode != newMode) {
        ADDLOG_DEBUG(LOG_FEATURE_CMD, "Changing LightMode from %s to %s",
			GetLightModeStr(g_lightMode),
			GetLightModeStr(newMode));
		g_lightMode = newMode;
	}
}
OBK_Publish_Result LED_SendCurrentLightMode() {

	if(g_lightMode == Light_Temperature) {
		return sendTemperatureChange();
	} else if(g_lightMode == Light_RGB) {
		return sendColorChange();
	}
	return OBK_PUBLISH_WAS_NOT_REQUIRED;
}
void LED_SetTemperature0to1Range(float f) {
	led_temperature_current = led_temperature_min + (led_temperature_max-led_temperature_min) * f;
}
float LED_GetTemperature0to1Range() {
	float f;

	f = (led_temperature_current - led_temperature_min);
	f = f / (led_temperature_max - led_temperature_min);
	if(f<0)
		f = 0;
	if(f>1)
		f =1;

	return f;
}
void LED_SetTemperature(int tmpInteger, bool bApply) {
	float f;

	led_temperature_current = tmpInteger;

	f = LED_GetTemperature0to1Range();

	baseColors[3] = (255.0f) * (1-f);
	baseColors[4] = (255.0f) * f;

	if(bApply) {
		// set g_lightMode
		SET_LightMode(Light_Temperature);
		sendTemperatureChange();
		apply_smart_light();
	}

}

static commandResult_t temperature(const void *context, const char *cmd, const char *args, int cmdFlags){
	int tmp;
	//if (!wal_strnicmp(cmd, "POWERALL", 8)){

        ADDLOG_DEBUG(LOG_FEATURE_CMD, " temperature (%s) received with args %s",cmd,args);

		Tokenizer_TokenizeString(args, 0);

		tmp = Tokenizer_GetArgInteger(0);

		LED_SetTemperature(tmp, 1);

		return CMD_RES_OK;
	//}
	//return 0;
}
OBK_Publish_Result LED_SendEnableAllState() {
	return MQTT_PublishMain_StringInt_DeDuped(DEDUP_LED_ENABLEALL,DEDUP_EXPIRE_TIME,"led_enableAll",g_lightEnableAll,0);
}

void LED_ToggleEnabled() {
	LED_SetEnableAll(!g_lightEnableAll);
}
bool g_guard_led_enable_event_cast = false;

void LED_SetEnableAll(int bEnable) {
	bool bEnableAllWasSetTo1;

	if (g_lightEnableAll == 0 && bEnable == 1) {
		bEnableAllWasSetTo1 = true;
	}
	else {
		bEnableAllWasSetTo1 = false;
	}

	// was there a change?
	if (g_lightEnableAll != bEnable) {
		// do not cast events recursively...
		// TODO: better fix
		if (g_guard_led_enable_event_cast == false) {
			g_guard_led_enable_event_cast = true;
			// cast event
			if (bEnable) {
				EventHandlers_FireEvent(CMD_EVENT_LED_STATE, 1);
			}
			else {
				EventHandlers_FireEvent(CMD_EVENT_LED_STATE, 0);
			}
			g_guard_led_enable_event_cast = false;
		}
	}
	g_lightEnableAll = bEnable;

	apply_smart_light();
#ifndef OBK_DISABLE_ALL_DRIVERS
	DRV_DGR_OnLedEnableAllChange(bEnable);
#endif
	LED_SendEnableAllState();
	if (bEnableAllWasSetTo1) {
		// if enable all was set to 1 this frame, also send dimmer
		// https://github.com/openshwprojects/OpenBK7231T_App/issues/498
		// TODO: check if it's OK 
		LED_SendDimmerChange();
	}
}
int LED_GetEnableAll() {
	return g_lightEnableAll;
}
static commandResult_t enableAll(const void *context, const char *cmd, const char *args, int cmdFlags){
	//if (!wal_strnicmp(cmd, "POWERALL", 8)){
		int bEnable;
		const char *a;
        ADDLOG_DEBUG(LOG_FEATURE_CMD, " enableAll (%s) received with args %s",cmd,args);

		Tokenizer_TokenizeString(args, 0);

		a = Tokenizer_GetArg(0);
		if (a && !stricmp(a, "toggle")) {
			bEnable = !g_lightEnableAll;
		}
		else {
			bEnable = Tokenizer_GetArgInteger(0);
		}

		LED_SetEnableAll(bEnable);


	//	sendColorChange();
	//	sendDimmerChange();
	//	sendTemperatureChange();

		return CMD_RES_OK;
	//}
	//return 0;
}
int LED_IsRunningDriver() {
	if(PIN_CountPinsWithRoleOrRole(IOR_PWM,IOR_PWM_n))
		return 1;
	if(CFG_HasFlag(OBK_FLAG_LED_FORCESHOWRGBCWCONTROLLER))
		return 1;
	return 0;
}
float LED_GetDimmer() {
	return g_brightness / g_cfg_brightnessMult;
}
void LED_AddTemperature(int iVal, bool wrapAroundInsteadOfClamp) {
	float cur;

	cur = led_temperature_current;

	cur += iVal;

	if (wrapAroundInsteadOfClamp == 0) {
		if (cur < led_temperature_min)
			cur = led_temperature_min;
		if (cur > led_temperature_max)
			cur = led_temperature_max;
	}
	else {
		if (cur < led_temperature_min)
			cur = led_temperature_max;
		if (cur > led_temperature_max)
			cur = led_temperature_min;
	}

	LED_SetTemperature(cur, true);
}
void LED_AddDimmer(int iVal, bool wrapAroundInsteadOfClamp, int minValue) {
	float cur;

	cur = g_brightness / g_cfg_brightnessMult;

	cur += iVal;

	if(wrapAroundInsteadOfClamp == 0) {
		if(cur < minValue)
			cur = minValue;
		if(cur > 100)
			cur = 100;
	} else {
		if(cur < minValue)
			cur = 100;
		if(cur > 100)
			cur = minValue;
	}

	LED_SetDimmer(cur);
}
void LED_NextTemperatureHold() {
	LED_AddTemperature(25, true);
}
void LED_NextDimmerHold() {
	// dimmer hold will use some kind of min value,
	// because it's easy to get confused if we set accidentally dimmer to 0
	// and then are unable to turn on the bulb (because despite of led_enableAll 1
	// the dimmer is 0 and anyColor * 0 gives 0)
	LED_AddDimmer(10, true, 2);
}
void LED_SetDimmer(int iVal) {

	g_brightness = iVal * g_cfg_brightnessMult;

#ifndef OBK_DISABLE_ALL_DRIVERS
	DRV_DGR_OnLedDimmerChange(iVal);
#endif

	apply_smart_light();
	LED_SendDimmerChange();

	if(shouldSendRGB()) {
		if(CFG_HasFlag(OBK_FLAG_MQTT_BROADCASTLEDPARAMSTOGETHER)) {
			sendColorChange();
		}
		if(CFG_HasFlag(OBK_FLAG_MQTT_BROADCASTLEDFINALCOLOR)) {
			sendFinalColor();
		}
	}

}
static commandResult_t add_dimmer(const void *context, const char *cmd, const char *args, int cmdFlags){
	int iVal = 0;
	int bWrapAroundInsteadOfHold;

	Tokenizer_TokenizeString(args, 0);

	iVal = Tokenizer_GetArgInteger(0);
	bWrapAroundInsteadOfHold = Tokenizer_GetArgInteger(1);

	LED_AddDimmer(iVal, bWrapAroundInsteadOfHold, 0);

	return CMD_RES_OK;
}
static commandResult_t dimmer(const void *context, const char *cmd, const char *args, int cmdFlags){
	//if (!wal_strnicmp(cmd, "POWERALL", 8)){
		int iVal = 0;

        ADDLOG_DEBUG(LOG_FEATURE_CMD, " dimmer (%s) received with args %s",cmd,args);

		// according to Elektroda.com users, domoticz sends following string:
		// {"brightness":52,"state":"ON"}
		if(args[0] == '{') {
			cJSON *json;
			const cJSON *brightness = NULL;
			const cJSON *state = NULL;

			json = cJSON_Parse(args);

			if(json == 0) {
				ADDLOG_INFO(LOG_FEATURE_CMD, "Dimmer - failed cJSON_Parse");
			} else {
				brightness = cJSON_GetObjectItemCaseSensitive(json, "brightness");
				if (brightness != 0 && cJSON_IsNumber(brightness))
				{
					ADDLOG_INFO(LOG_FEATURE_CMD, "Dimmer - cJSON_Parse says brightness is %i",brightness->valueint);

					LED_SetDimmer(brightness->valueint);
				}
				state = cJSON_GetObjectItemCaseSensitive(json, "state");
				if (state != 0 && cJSON_IsString(state) && (state->valuestring != NULL))
				{
					ADDLOG_INFO(LOG_FEATURE_CMD, "Dimmer - cJSON_Parse says state is %s",state->valuestring);

					if(!stricmp(state->valuestring,"ON")) {
						LED_SetEnableAll(true);
					} else if(!stricmp(state->valuestring,"OFF")) {
						LED_SetEnableAll(false);
					} else {

					}
				}
				cJSON_Delete(json);
			}
		} else {
			Tokenizer_TokenizeString(args, 0);

			iVal = Tokenizer_GetArgInteger(0);

			LED_SetDimmer(iVal);
		}

		return 1;
	//}
	//return 0;
}
void LED_SetFinalRGBCW(byte *rgbcw) {
	if(rgbcw[0] == 0 && rgbcw[1] == 0 && rgbcw[2] == 0 && rgbcw[3] == 0 && rgbcw[4] == 0) {

	}

	if(rgbcw[3] == 0 && rgbcw[4] == 0) {
		LED_SetFinalRGB(rgbcw[0],rgbcw[1],rgbcw[2]);
	} else {
		LED_SetFinalCW(rgbcw[3],rgbcw[4]);
	}
}
void LED_GetFinalChannels100(byte *rgbcw) {
	rgbcw[0] = finalColors[0] * (100.0f / 255.0f);
	rgbcw[1] = finalColors[1] * (100.0f / 255.0f);
	rgbcw[2] = finalColors[2] * (100.0f / 255.0f);
	rgbcw[3] = finalColors[3] * (100.0f / 255.0f);
	rgbcw[4] = finalColors[4] * (100.0f / 255.0f);
}
void LED_GetFinalHSV(int *hsv) {
	hsv[0] = g_hsv_h;
	hsv[1] = g_hsv_s;
	hsv[2] = g_hsv_v;
}
void LED_GetFinalRGBCW(byte *rgbcw) {
	rgbcw[0] = finalColors[0];
	rgbcw[1] = finalColors[1];
	rgbcw[2] = finalColors[2];
	rgbcw[3] = finalColors[3];
	rgbcw[4] = finalColors[4];
}
void LED_SetFinalCW(byte c, byte w) {
	float tmp;

	SET_LightMode(Light_Temperature);

	// TODO: finish the calculation,
	// the Device Group sent as White and Cool values in byte range,
	// we need to get back Temperature value
	tmp = c / 255.0f;

	LED_SetTemperature0to1Range(tmp);

	baseColors[3] = c;
	baseColors[4] = w;

	apply_smart_light();
}
void LED_SetFinalRGB(byte r, byte g, byte b) {
	SET_LightMode(Light_RGB);

	baseColors[0] = r;
	baseColors[1] = g;
	baseColors[2] = b;

	RGBtoHSV(baseColors[0]/255.0f, baseColors[1]/255.0f, baseColors[2]/255.0f, &g_hsv_h, &g_hsv_s, &g_hsv_v);

	apply_smart_light();

	// TODO
	if(0) {
		sendColorChange();
		if(CFG_HasFlag(OBK_FLAG_MQTT_BROADCASTLEDPARAMSTOGETHER)) {
			LED_SendDimmerChange();
		}
		if(CFG_HasFlag(OBK_FLAG_MQTT_BROADCASTLEDFINALCOLOR)) {
			sendFinalColor();
		}
	}
}
static void onHSVChanged() {
	float r, g, b;

	HSVtoRGB(&r, &g, &b, g_hsv_h, g_hsv_s, g_hsv_v);

	baseColors[0] = r * 255.0f;
	baseColors[1] = g * 255.0f;
	baseColors[2] = b * 255.0f;

	sendColorChange();

	apply_smart_light();


	if (CFG_HasFlag(OBK_FLAG_MQTT_BROADCASTLEDFINALCOLOR)) {
		sendFinalColor();
	}
}
commandResult_t LED_SetBaseColor_HSB(const void *context, const char *cmd, const char *args, int bAll) {
	int hue, sat, bri;
	const char *p;

	Tokenizer_TokenizeString(args, 0);
	if (Tokenizer_GetArgsCount() == 1) {
		p = args;
		hue = atoi(p);
		while (*p) {
			if (*p == ',')
			{
				p++;
				break;
			}
			p++;
		}
		sat = atoi(p);
		while (*p) {
			if (*p == ',')
			{
				p++;
				break;
			}
			p++;
		}
		bri = atoi(p);
	}
	else {
		hue = Tokenizer_GetArgInteger(0);
		sat = Tokenizer_GetArgInteger(1);
		bri = Tokenizer_GetArgInteger(2);
	}

	SET_LightMode(Light_RGB);

	g_hsv_h = hue;
	g_hsv_s = sat * 0.01f;
	g_hsv_v = bri * 0.01f;

	onHSVChanged();

	return CMD_RES_OK;
}
commandResult_t LED_SetBaseColor(const void *context, const char *cmd, const char *args, int bAll){
   // support both '#' prefix and not
            const char *c = args;
            int val = 0;
            ADDLOG_DEBUG(LOG_FEATURE_CMD, " BASECOLOR got %s", args);

			// some people prefix colors with #
			if(c[0] == '#')
				c++;

			if(bAll) {
				SET_LightMode(Light_All);
			} else {
				SET_LightMode(Light_RGB);
			}

			g_numBaseColors = 0;
			if(!stricmp(c,"rand")) {
				baseColors[0] = rand()%255;
				baseColors[1] = rand()%255;
				baseColors[2] = rand()%255;
				if(bAll){
					baseColors[3] = rand()%255;
					baseColors[4] = rand()%255;
				}
			} else {
				while (*c && g_numBaseColors < 5){
					char tmp[3];
					int r;
					tmp[0] = *(c++);
					if (!*c)
						break;
					tmp[1] = *(c++);
					tmp[2] = '\0';
					r = sscanf(tmp, "%x", &val);
					if (!r) {
						ADDLOG_ERROR(LOG_FEATURE_CMD, "BASECOLOR no sscanf hex result from %s", tmp);
						break;
					}


					//ADDLOG_DEBUG(LOG_FEATURE_CMD, "BASECOLOR found chan %d -> val255 %d (from %s)", g_numBaseColors, val, tmp);

					baseColors[g_numBaseColors] = val;
				//	baseColorChannels[g_numBaseColors] = channel;
					g_numBaseColors++;

				}
				// keep hsv in sync
			}

			RGBtoHSV(baseColors[0]/255.0f, baseColors[1]/255.0f, baseColors[2]/255.0f, &g_hsv_h, &g_hsv_s, &g_hsv_v);

			apply_smart_light();
			sendColorChange();
			if(CFG_HasFlag(OBK_FLAG_MQTT_BROADCASTLEDPARAMSTOGETHER)) {
				LED_SendDimmerChange();
			}
			if(CFG_HasFlag(OBK_FLAG_MQTT_BROADCASTLEDFINALCOLOR)) {
				sendFinalColor();
			}

        return CMD_RES_OK;
  //  }
   // return 0;
}

static commandResult_t basecolor_rgb(const void *context, const char *cmd, const char *args, int cmdFlags){
	return LED_SetBaseColor(context,cmd,args,0);
}
static commandResult_t basecolor_rgbcw(const void *context, const char *cmd, const char *args, int cmdFlags){
	return LED_SetBaseColor(context,cmd,args,1);
}

// CONFIG-ONLY command!
static commandResult_t colorMult(const void *context, const char *cmd, const char *args, int cmdFlags){
        ADDLOG_DEBUG(LOG_FEATURE_CMD, " g_cfg_colorScaleToChannel (%s) received with args %s",cmd,args);

		g_cfg_colorScaleToChannel = atof(args);

		return CMD_RES_OK;
	//}
	//return 0;
}
// CONFIG-ONLY command!
static commandResult_t brightnessMult(const void *context, const char *cmd, const char *args, int cmdFlags){
        ADDLOG_DEBUG(LOG_FEATURE_CMD, " brightnessMult (%s) received with args %s",cmd,args);

		g_cfg_brightnessMult = atof(args);

		return CMD_RES_OK;
	//}
	//return 0;
}
float LED_GetGreen255() {
	return baseColors[1];
}
float LED_GetRed255() {
	return baseColors[0];
}
float LED_GetBlue255() {
	return baseColors[2];
}
static void led_setBrightness(float sat) {

	g_hsv_v = sat;

	onHSVChanged();
}
static void led_setSaturation(float sat){

	g_hsv_s = sat;

	onHSVChanged();
}
static void led_setHue(float hue){

	g_hsv_h = hue;

	onHSVChanged();
}
static commandResult_t nextColor(const void *context, const char *cmd, const char *args, int cmdFlags){
   
	LED_NextColor();

	return CMD_RES_OK;
}
static commandResult_t lerpSpeed(const void *context, const char *cmd, const char *args, int cmdFlags){
	// Use tokenizer, so we can use variables (eg. $CH11 as variable)
	Tokenizer_TokenizeString(args, 0);

	led_lerpSpeedUnitsPerSecond = Tokenizer_GetArgFloat(0);

	return CMD_RES_OK;
}
static commandResult_t setBrightness(const void *context, const char *cmd, const char *args, int cmdFlags) {
	float f;

	// Use tokenizer, so we can use variables (eg. $CH11 as variable)
	Tokenizer_TokenizeString(args, 0);

	f = Tokenizer_GetArgFloat(0);

	// input is in 0-100 range
	f *= 0.01f;

	SET_LightMode(Light_RGB);

	led_setBrightness(f);

	return CMD_RES_OK;
}
static commandResult_t setSaturation(const void *context, const char *cmd, const char *args, int cmdFlags){
    float f;

	// Use tokenizer, so we can use variables (eg. $CH11 as variable)
	Tokenizer_TokenizeString(args, 0);

	f = Tokenizer_GetArgFloat(0);

	// input is in 0-100 range
	f *= 0.01f;

	SET_LightMode(Light_RGB);

	led_setSaturation(f);

	return CMD_RES_OK;
}
float LED_GetSaturation() {
	return g_hsv_s * 100.0f;
}
static commandResult_t setHue(const void *context, const char *cmd, const char *args, int cmdFlags){
    float f;

	// Use tokenizer, so we can use variables (eg. $CH11 as variable)
	Tokenizer_TokenizeString(args, 0);

	f = Tokenizer_GetArgFloat(0);

	SET_LightMode(Light_RGB);

	led_setHue(f);

	return CMD_RES_OK;
}

float LED_GetHue() {
	return g_hsv_h;
}
void NewLED_InitCommands(){
	// set, but do not apply (force a refresh)
	LED_SetTemperature(led_temperature_current,0);

	// if this is CW, switch from default RGB to CW
	if (isCWMode()) {
		g_lightMode = Light_Temperature;
	}

	//cmddetail:{"name":"led_dimmer","args":"",
	//cmddetail:"descr":"set output dimmer 0..100",
	//cmddetail:"fn":"dimmer","file":"cmnds/cmd_newLEDDriver.c","requires":"",
	//cmddetail:"examples":""}
    CMD_RegisterCommand("led_dimmer", "", dimmer, NULL, NULL);
	//cmddetail:{"name":"add_dimmer","args":"",
	//cmddetail:"descr":"set output dimmer 0..100",
	//cmddetail:"fn":"add_dimmer","file":"cmnds/cmd_newLEDDriver.c","requires":"",
	//cmddetail:"examples":""}
    CMD_RegisterCommand("add_dimmer", "", add_dimmer, NULL, NULL);
	//cmddetail:{"name":"led_enableAll","args":"",
	//cmddetail:"descr":"qqqq",
	//cmddetail:"fn":"enableAll","file":"cmnds/cmd_newLEDDriver.c","requires":"",
	//cmddetail:"examples":""}
    CMD_RegisterCommand("led_enableAll", "", enableAll, NULL, NULL);
	//cmddetail:{"name":"led_basecolor_rgb","args":"",
	//cmddetail:"descr":"set PWN color using #RRGGBB",
	//cmddetail:"fn":"basecolor_rgb","file":"cmnds/cmd_newLEDDriver.c","requires":"",
	//cmddetail:"examples":""}
    CMD_RegisterCommand("led_basecolor_rgb", "", basecolor_rgb, NULL, NULL);
	//cmddetail:{"name":"led_basecolor_rgbcw","args":"",
	//cmddetail:"descr":"set PWN color using #RRGGBB[cw][ww]",
	//cmddetail:"fn":"basecolor_rgbcw","file":"cmnds/cmd_newLEDDriver.c","requires":"",
	//cmddetail:"examples":""}
    CMD_RegisterCommand("led_basecolor_rgbcw", "", basecolor_rgbcw, NULL, NULL);
	//cmddetail:{"name":"led_temperature","args":"",
	//cmddetail:"descr":"set qqqq",
	//cmddetail:"fn":"temperature","file":"cmnds/cmd_newLEDDriver.c","requires":"",
	//cmddetail:"examples":""}
    CMD_RegisterCommand("led_temperature", "", temperature, NULL, NULL);
	//cmddetail:{"name":"led_brightnessMult","args":"",
	//cmddetail:"descr":"set qqqq",
	//cmddetail:"fn":"brightnessMult","file":"cmnds/cmd_newLEDDriver.c","requires":"",
	//cmddetail:"examples":""}
    CMD_RegisterCommand("led_brightnessMult", "", brightnessMult, NULL, NULL);
	//cmddetail:{"name":"led_colorMult","args":"",
	//cmddetail:"descr":"set qqqq",
	//cmddetail:"fn":"colorMult","file":"cmnds/cmd_newLEDDriver.c","requires":"",
	//cmddetail:"examples":""}
    CMD_RegisterCommand("led_colorMult", "", colorMult, NULL, NULL);
	//cmddetail:{"name":"led_saturation","args":"",
	//cmddetail:"descr":"set qqqq",
	//cmddetail:"fn":"setSaturation","file":"cmnds/cmd_newLEDDriver.c","requires":"",
	//cmddetail:"examples":""}
    CMD_RegisterCommand("led_saturation", "", setSaturation, NULL, NULL);
	//cmddetail:{"name":"led_hue","args":"",
	//cmddetail:"descr":"set qqqq",
	//cmddetail:"fn":"setHue","file":"cmnds/cmd_newLEDDriver.c","requires":"",
	//cmddetail:"examples":""}
    CMD_RegisterCommand("led_hue", "", setHue, NULL, NULL);
	//cmddetail:{"name":"led_nextColor","args":"",
	//cmddetail:"descr":"set qqqq",
	//cmddetail:"fn":"nextColor","file":"cmnds/cmd_newLEDDriver.c","requires":"",
	//cmddetail:"examples":""}
    CMD_RegisterCommand("led_nextColor", "", nextColor, NULL, NULL);
	//cmddetail:{"name":"led_lerpSpeed","args":"",
	//cmddetail:"descr":"set qqqq",
	//cmddetail:"fn":"lerpSpeed","file":"cmnds/cmd_newLEDDriver.c","requires":"",
	//cmddetail:"examples":""}
    CMD_RegisterCommand("led_lerpSpeed", "", lerpSpeed, NULL, NULL);
	//cmddetail:{"name":"led_expoMode","args":"",
	//cmddetail:"descr":"set brightness exponential mode 0..4",
	//cmddetail:"fn":"exponentialMode","file":"cmnds/cmd_newLEDDriver.c","requires":"",
	//cmddetail:"examples":"led_expoMode 4"}
    CMD_RegisterCommand("led_expoMode", "", exponentialMode, NULL, NULL);

	// HSBColor 360,100,100 - red
	// HSBColor 90,100,100 - green
	// HSBColor	<hue>,<sat>,<bri> = set color by hue, saturation and brightness
	CMD_RegisterCommand("HSBColor", "", LED_SetBaseColor_HSB, NULL, NULL);
	// HSBColor1	0..360 = set hue
	CMD_RegisterCommand("HSBColor1", "", setHue, NULL, NULL);
	// HSBColor2	0..100 = set saturation
	CMD_RegisterCommand("HSBColor2", "", setSaturation, NULL, NULL);
	// HSBColor3	0..100 = set brightness
	CMD_RegisterCommand("HSBColor3", "", setBrightness, NULL, NULL);
}

void NewLED_RestoreSavedStateIfNeeded() {
	if(CFG_HasFlag(OBK_FLAG_LED_REMEMBERLASTSTATE)) {
		short brig;
		short tmp;
		byte rgb[3];
		byte mod;
		byte bEnableAll;

		HAL_FlashVars_ReadLED(&mod, &brig, &tmp, rgb, &bEnableAll);

		g_lightEnableAll = bEnableAll;
		SET_LightMode(mod);
		g_brightness = brig * g_cfg_brightnessMult;
		LED_SetTemperature(tmp,0);
		baseColors[0] = rgb[0];
		baseColors[1] = rgb[1];
		baseColors[2] = rgb[2];
		apply_smart_light();
	} else {
	}

	// "cmnd/obk8D38570E/led_dimmer_get""
}
