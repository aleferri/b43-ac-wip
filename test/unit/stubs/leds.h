/* SPDX-License-Identifier: GPL-2.0 */
#ifndef B43_TEST_STUB_LEDS_H_
#define B43_TEST_STUB_LEDS_H_

/* From drivers/net/wireless/broadcom/b43/leds.h. */
#define B43_LED_BEHAVIOUR		0x7F
#define B43_LED_ACTIVELOW		0x80

enum b43_led_behaviour {
	B43_LED_OFF,
	B43_LED_ON,
	B43_LED_ACTIVITY,
	B43_LED_RADIO_ALL,
	B43_LED_RADIO_A,
	B43_LED_RADIO_B,
	B43_LED_MODE_BG,
	B43_LED_TRANSFER,
	B43_LED_APTRANSFER,
	B43_LED_WEIRD,
	B43_LED_ASSOC,
	B43_LED_INACTIVE,
};

#endif
