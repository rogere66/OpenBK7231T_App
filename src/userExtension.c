/*
	userExtension.c - extension code added by end-user:
		- LED light control using potmeter connected to ADC input and MQTT group commands
		- adding "saveLED" command to save power-up LED state

	Setup:
		- call userExtension_init() from some task/thread
		Further instructions for each function in code
*/

#include "logging/logging.h"
#include "cmnds/cmd_local.h"
#include "new_pins.h"
#include "hal/hal_adc.h"
#include "mqtt/new_mqtt.h"
#include "new_cfg.h"

// external references not available in include files: 
OBK_Publish_Result MQTT_Publish_RawStringInt (char* sBaseTopic, char* sTopic, int iVal, int flags);
void apply_smart_light (void);

/*
	LED light control using potmeter connected to ADC input (tested only on BK7231T).
	The ADC reading is filtered and scaled down to range 0-100, suitable for LED brightness.
	To reduce delays the output value is published using MQTT group commands - the clients can
	then subscribe to the group by defining Group Topic in MQTT Config.

	Setup:
		add the following include statement to the end of new_mqtt.c:

#include "../userExtension_mqtt.h" // additional code used by userExtension.c

*/
void userExtension_thread (beken_thread_arg_t arg) // local timer task
{
	unsigned int publishTimer = 0;
	char sBaseTopic[100];
	int publishStatus = !OBK_PUBLISH_OK;
	int adcFlt = 30700;
	int prevAdcFlt = 0;
	int prevOutVal = -1;
	while (1) {
		// read ADC, filter and publish if ADC on pin 23 (only ADC pin on BK7231), output range 0-100:
		if (g_cfg.pins.roles[23] == IOR_ADC) {
			publishTimer++;
			TickType_t currentTickCount = xTaskGetTickCount ();
			int adcVal = HAL_ADC_Read (23);
			if (adcVal >= 0) {
				int outVal;
				adcFlt = adcFlt + adcVal - adcFlt / 15; // adcFlt range = 14-61425 (0-4095 from ADC)
				outVal = (adcFlt + 307) / 614;          // adjust output range to 0-100
				if ((outVal != prevOutVal) && (outVal != 0) && (outVal != 100) && (abs (adcFlt - prevAdcFlt) < 307)) {
					outVal = prevOutVal; // ignore small changes (noise)
				}
				if ((publishTimer >= 6) && ((outVal != prevOutVal) || (publishStatus != OBK_PUBLISH_OK))) {
					publishTimer = 0;
					prevOutVal = outVal;
					prevAdcFlt = adcFlt;
					sprintf (sBaseTopic, "cmnd/%s_ADC", CFG_GetMQTTClientId());
					publishStatus = MQTT_Publish_RawStringInt (sBaseTopic, "led_dimmer", outVal, OBK_PUBLISH_FLAG_RETAIN);
				}
				/*/ debug PO
				static int secCnt;
				if (secCnt++ > 200) {
					secCnt = 0;
					addLogAdv (LOG_INFO, LOG_FEATURE_RAW, "ADC filter o=%i, p=%i: a=%i, f=%i, p=%i, d=%i",
					           outVal, prevOutVal, adcVal, adcFlt, prevAdcFlt, adcFlt - prevAdcFlt);
				} // */
			}
			vTaskDelayUntil (&currentTickCount, 12); // 24 mS
		} else {
			vTaskDelay (1000);
		}
	}
}

/*
	Add "saveLED" command to save power-up LED state. This feature use "Flag 12 - [LED] Remember LED driver state"
	to remember a fixed LED setting (rather than last used setting). Set Flag 12 and adjust the LED to desired
	power-up setting, then issue the saveLED command - this LED setting will then be used at power-up.

	Setup:
		Modify the HAL_FlashVars_SaveLED() call in cmd_newLEDDriver.c to look something like this:

		extern int saveLED; // modified power up functionality - check userExtension.c for details
		if (saveLED) {
			saveLED = 0;
			HAL_FlashVars_SaveLED(g_lightMode, iBrightness0to100, led_temperature_current,baseColors[0],baseColors[1],baseColors[2],g_lightEnableAll);
		}
 */
int saveLED = 0;
static commandResult_t userExtension_command (const void *context, const char *cmd, const char *args, int cmdFlags) {
	saveLED = 1;
	apply_smart_light ();
	return CMD_RES_OK;
}

void userExtension_init (void)
{
	CMD_RegisterCommand ("saveLED", userExtension_command, NULL);
	xTaskCreate (userExtension_thread, "userExt_thread", 0x800, NULL, BEKEN_APPLICATION_PRIORITY, NULL);
	logfeatures &= ~0x000000c0; // turn off some log messages if not needed, LOG_FEATURE_: GEN MAIN (MQTT add 2)
}
