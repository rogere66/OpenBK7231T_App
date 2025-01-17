#ifdef WINDOWS
#include "Controller_BL0942.h"
#include "Shape.h"
#include "Junction.h"
#include "Text.h"

#define BL0942_PACKET_LEN 23
#define BL0942_READ_COMMAND 0x58
static float BL0942_PREF = 598;
static float BL0942_UREF = 15188;
static float BL0942_IREF = 251210;

void CControllerBL0942::onDrawn() {
	int currentPending = UART_GetDataSize();
	if (currentPending > 0) {
		return;
	}
	
	// Now you must do it by hand in simulation (or put in autoexec.bat)
	//CMD_ExecuteCommand("startDriver BL0942", 0);

	if (txt_voltage->isBeingEdited() == false) {
		realVoltage = txt_voltage->getFloat();
	}
	if (txt_power->isBeingEdited() == false) {
		realPower = txt_power->getFloat();
	}
	if (txt_current->isBeingEdited() == false) {
		realCurrent = txt_current->getFloat();
	}
	int i;
	byte data[BL0942_PACKET_LEN];
	memset(data, 0, sizeof(data));
	data[0] = 0x55;
	byte checksum = BL0942_READ_COMMAND;

	int bl_current = BL0942_IREF * realCurrent;
	int bl_power = BL0942_PREF * realPower;
	int bl_voltage = BL0942_UREF * realVoltage;

	data[1] = (byte)(bl_current);
	data[2] = (byte)(bl_current >> 8);
	data[3] = (byte)(bl_current >> 16);

	data[4] = (byte)(bl_voltage);
	data[5] = (byte)(bl_voltage >> 8);
	data[6] = (byte)(bl_voltage >> 16);

	data[10] = (byte)(bl_power);
	data[11] = (byte)(bl_power >> 8);
	data[12] = (byte)(bl_power >> 16);
	for (i = 0; i < BL0942_PACKET_LEN - 1; i++) {
		checksum += data[i];
	}
	checksum ^= 0xFF;
	data[BL0942_PACKET_LEN - 1] = checksum;
	for (i = 0; i < BL0942_PACKET_LEN; i++) {
		UART_AppendByteToCircularBuffer(data[i]);
	}
}
class CControllerBase *CControllerBL0942::cloneController(class CShape *origOwner, class CShape *newOwner) {
	CControllerBL0942 *r = new CControllerBL0942();
	if (tx) {
		r->tx = newOwner->findShapeByName_r(tx->getName())->asJunction();
	}
	if (rx) {
		r->rx = newOwner->findShapeByName_r(rx->getName())->asJunction();
	}
	if (txt_voltage) {
		r->txt_voltage = newOwner->findShapeByName_r(txt_voltage->getName())->asText();
	}
	if (txt_current) {
		r->txt_current = newOwner->findShapeByName_r(txt_current->getName())->asText();
	}
	if (txt_power) {
		r->txt_power = newOwner->findShapeByName_r(txt_power->getName())->asText();
	}
	return r;
}

#endif
