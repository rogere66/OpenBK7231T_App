/*
	userExtension_mqtt.h - additional MQTT function used by userExtension.c

	Add the following include statement to the end of new_mqtt.c:

#include "../userExtension_mqtt.h" // additional code used by userExtension.c

*/

// Publish MQTT group command
OBK_Publish_Result MQTT_Publish_RawStringInt (char* sBaseTopic, char* sTopic, int iVal, int flags)
{
	char valueStr[16];
	sprintf (valueStr, "%i", iVal);
	return MQTT_PublishTopicToClient (mqtt_client, sBaseTopic, sTopic, valueStr, flags, 0);
}
