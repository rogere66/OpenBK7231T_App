/*
	sysView.c - additional commands to display some useful system information:
		- showTasks - list FreeRTOS tasks with State, Priority, Stack headroom, Task number
		- showStats - list FreeRTOS run-time statistics with Time/% used by each task
		- showCommands - list all registered OBK commands
		- help - sysView command list and some other info

	Setup:
		- call sysView_init() from some task/thread to add the commands
		- add/modify the following code in FreeRTOSConfig.h (required for showStats):

void sysViewStatsTimerInit (void); // extension used by sysView.c
extern volatile unsigned int sysViewStatsTimer;
#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS()  sysViewStatsTimerInit()
#define portGET_RUN_TIME_COUNTER_VALUE()          sysViewStatsTimer
#define configGENERATE_RUN_TIME_STATS             1

*/

#include "logging/logging.h"
#include "cmnds/cmd_local.h"

static int listLineCount = 0;
static void listCallback (command_t *newCmd, void *userData)
{
	addLogAdv (LOG_INFO, LOG_FEATURE_RAW, " %3i %s", ++listLineCount, newCmd->name);
}

volatile unsigned int sysViewStatsTimer = 0;
static void sysViewStatsTimerISR (void)
{
	sysViewStatsTimer++;
}

void sysViewStatsTimerInit (void)
{
	sysViewStatsTimer = 0;
	OSStatus bk_timer_initialize_us (uint8_t timer_id, uint32_t time_us, void *callback);
	bk_timer_initialize_us (1, 100, sysViewStatsTimerISR);
}

static commandResult_t sysView_command (const void *context, const char *cmd, const char *args, int cmdFlags) {
	if (strcmp ("showTasks", cmd) == 0) {
		char buf[800];
		vTaskList (buf);
		addLogAdv (LOG_INFO, LOG_FEATURE_RAW, "\r\nTask Name     State  Priority	Stack	Num\r\n%s", buf);
	} else if (strcmp ("showStats", cmd) == 0) {
		char buf[800];
		vTaskGetRunTimeStats (buf);
		addLogAdv (LOG_INFO, LOG_FEATURE_RAW, "\r\nTask Name    Time [100uS]     % Time\r\n%s", buf);
	} else if (strcmp ("showCommands", cmd) == 0) {
		listLineCount = 0;
		addLogAdv (LOG_INFO, LOG_FEATURE_RAW, "\r\nOBK Commands:");
		CMD_ListAllCommands (NULL, listCallback);
	} else if (strcmp ("help", cmd) == 0) {
		TaskHandle_t thisTask = xTaskGetCurrentTaskHandle ();
		addLogAdv (LOG_INFO, LOG_FEATURE_RAW, "\r\nsysView commands:  showTasks  showStats  showCommands  help");
		addLogAdv (LOG_INFO, LOG_FEATURE_RAW, "FreeRTOS version %s", tskKERNEL_VERSION_NUMBER);
		addLogAdv (LOG_INFO, LOG_FEATURE_RAW, "sysView is running in task %s", pcTaskGetName (thisTask));
	}
	return CMD_RES_OK;
}

void sysView_init (void)
{
	CMD_RegisterCommand("showTasks", sysView_command, NULL);
	CMD_RegisterCommand("showStats", sysView_command, NULL);
	CMD_RegisterCommand("showCommands", sysView_command, NULL);
	CMD_RegisterCommand("help", sysView_command, NULL);

	// also initialize userExtension.c, if used:
	void userExtension_init (void);
	userExtension_init ();
}
