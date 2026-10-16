/* HID boot-protocol keyboard and relative mouse interfaces. */
#include <device/usb/usb.h>
#include <lib/klib.h>
#include <errno.h>
#include "hid_protocol.h"
#include <macro.h>

struct hid {
	struct usb_interface *interface;
	struct usb_endpoint *in;
	struct hid_boot_keyboard_report previous;
	unsigned repeat;
	uint64_t deadline;
	struct hid *next;
};
static struct hid *keyboards;
static mutex_t keyboards_lock;
static unsigned char keyboard_leds;
static void hid_init(void)
{
	mutex_init(&keyboards_lock);
}
KERNEL_INIT(1, hid_init);
/* USB keyboard usages mapped to set-1 make codes, with E0 in the high byte. */
static const unsigned short keys[HID_KEYBOARD_USAGES] = {
	[HID_USAGE_A] = SET1_KEY_A,
	[HID_USAGE_B] = SET1_KEY_B,
	[HID_USAGE_C] = SET1_KEY_C,
	[HID_USAGE_D] = SET1_KEY_D,
	[HID_USAGE_E] = SET1_KEY_E,
	[HID_USAGE_F] = SET1_KEY_F,
	[HID_USAGE_G] = SET1_KEY_G,
	[HID_USAGE_H] = SET1_KEY_H,
	[HID_USAGE_I] = SET1_KEY_I,
	[HID_USAGE_J] = SET1_KEY_J,
	[HID_USAGE_K] = SET1_KEY_K,
	[HID_USAGE_L] = SET1_KEY_L,
	[HID_USAGE_M] = SET1_KEY_M,
	[HID_USAGE_N] = SET1_KEY_N,
	[HID_USAGE_O] = SET1_KEY_O,
	[HID_USAGE_P] = SET1_KEY_P,
	[HID_USAGE_Q] = SET1_KEY_Q,
	[HID_USAGE_R] = SET1_KEY_R,
	[HID_USAGE_S] = SET1_KEY_S,
	[HID_USAGE_T] = SET1_KEY_T,
	[HID_USAGE_U] = SET1_KEY_U,
	[HID_USAGE_V] = SET1_KEY_V,
	[HID_USAGE_W] = SET1_KEY_W,
	[HID_USAGE_X] = SET1_KEY_X,
	[HID_USAGE_Y] = SET1_KEY_Y,
	[HID_USAGE_Z] = SET1_KEY_Z,
	[HID_USAGE_1] = SET1_KEY_1,
	[HID_USAGE_2] = SET1_KEY_2,
	[HID_USAGE_3] = SET1_KEY_3,
	[HID_USAGE_4] = SET1_KEY_4,
	[HID_USAGE_5] = SET1_KEY_5,
	[HID_USAGE_6] = SET1_KEY_6,
	[HID_USAGE_7] = SET1_KEY_7,
	[HID_USAGE_8] = SET1_KEY_8,
	[HID_USAGE_9] = SET1_KEY_9,
	[HID_USAGE_0] = SET1_KEY_0,
	[HID_USAGE_ENTER] = SET1_KEY_ENTER,
	[HID_USAGE_ESCAPE] = SET1_KEY_ESCAPE,
	[HID_USAGE_BACKSPACE] = SET1_KEY_BACKSPACE,
	[HID_USAGE_TAB] = SET1_KEY_TAB,
	[HID_USAGE_SPACE] = SET1_KEY_SPACE,
	[HID_USAGE_MINUS] = SET1_KEY_MINUS,
	[HID_USAGE_EQUAL] = SET1_KEY_EQUAL,
	[HID_USAGE_LEFT_BRACKET] = SET1_KEY_LEFT_BRACKET,
	[HID_USAGE_RIGHT_BRACKET] = SET1_KEY_RIGHT_BRACKET,
	[HID_USAGE_BACKSLASH] = SET1_KEY_BACKSLASH,
	[HID_USAGE_NONUS_HASH] = SET1_KEY_NONUS_HASH,
	[HID_USAGE_SEMICOLON] = SET1_KEY_SEMICOLON,
	[HID_USAGE_APOSTROPHE] = SET1_KEY_APOSTROPHE,
	[HID_USAGE_GRAVE] = SET1_KEY_GRAVE,
	[HID_USAGE_COMMA] = SET1_KEY_COMMA,
	[HID_USAGE_PERIOD] = SET1_KEY_PERIOD,
	[HID_USAGE_SLASH] = SET1_KEY_SLASH,
	[HID_USAGE_CAPS_LOCK] = SET1_KEY_CAPS_LOCK,
	[HID_USAGE_F1] = SET1_KEY_F1,
	[HID_USAGE_F2] = SET1_KEY_F2,
	[HID_USAGE_F3] = SET1_KEY_F3,
	[HID_USAGE_F4] = SET1_KEY_F4,
	[HID_USAGE_F5] = SET1_KEY_F5,
	[HID_USAGE_F6] = SET1_KEY_F6,
	[HID_USAGE_F7] = SET1_KEY_F7,
	[HID_USAGE_F8] = SET1_KEY_F8,
	[HID_USAGE_F9] = SET1_KEY_F9,
	[HID_USAGE_F10] = SET1_KEY_F10,
	[HID_USAGE_F11] = SET1_KEY_F11,
	[HID_USAGE_F12] = SET1_KEY_F12,
	[HID_USAGE_PRINT_SCREEN] = SET1_KEY_PRINT_SCREEN,
	[HID_USAGE_SCROLL_LOCK] = SET1_KEY_SCROLL_LOCK,
	[HID_USAGE_INSERT] = SET1_KEY_INSERT,
	[HID_USAGE_HOME] = SET1_KEY_HOME,
	[HID_USAGE_PAGE_UP] = SET1_KEY_PAGE_UP,
	[HID_USAGE_DELETE] = SET1_KEY_DELETE,
	[HID_USAGE_END] = SET1_KEY_END,
	[HID_USAGE_PAGE_DOWN] = SET1_KEY_PAGE_DOWN,
	[HID_USAGE_RIGHT] = SET1_KEY_RIGHT,
	[HID_USAGE_LEFT] = SET1_KEY_LEFT,
	[HID_USAGE_DOWN] = SET1_KEY_DOWN,
	[HID_USAGE_UP] = SET1_KEY_UP,
	[HID_USAGE_NUM_LOCK] = SET1_KEY_NUM_LOCK,
	[HID_USAGE_KP_SLASH] = SET1_KEY_KP_SLASH,
	[HID_USAGE_KP_STAR] = SET1_KEY_KP_STAR,
	[HID_USAGE_KP_MINUS] = SET1_KEY_KP_MINUS,
	[HID_USAGE_KP_PLUS] = SET1_KEY_KP_PLUS,
	[HID_USAGE_KP_ENTER] = SET1_KEY_KP_ENTER,
	[HID_USAGE_KP_1] = SET1_KEY_KP_1,
	[HID_USAGE_KP_2] = SET1_KEY_KP_2,
	[HID_USAGE_KP_3] = SET1_KEY_KP_3,
	[HID_USAGE_KP_4] = SET1_KEY_KP_4,
	[HID_USAGE_KP_5] = SET1_KEY_KP_5,
	[HID_USAGE_KP_6] = SET1_KEY_KP_6,
	[HID_USAGE_KP_7] = SET1_KEY_KP_7,
	[HID_USAGE_KP_8] = SET1_KEY_KP_8,
	[HID_USAGE_KP_9] = SET1_KEY_KP_9,
	[HID_USAGE_KP_0] = SET1_KEY_KP_0,
	[HID_USAGE_KP_PERIOD] = SET1_KEY_KP_PERIOD,
	[HID_USAGE_NONUS_BACKSLASH] = SET1_KEY_NONUS_BACKSLASH,
	[HID_USAGE_APPLICATION] = SET1_KEY_APPLICATION,
	[HID_USAGE_KP_EQUAL] = SET1_KEY_KP_EQUAL,
};
static const unsigned short modifiers[HID_KEYBOARD_MODIFIERS] = {
	SET1_KEY_LEFT_CTRL, SET1_KEY_LEFT_SHIFT, SET1_KEY_LEFT_ALT,
	SET1_KEY_LEFT_GUI,  SET1_KEY_RIGHT_CTRL, SET1_KEY_RIGHT_SHIFT,
	SET1_KEY_RIGHT_ALT, SET1_KEY_RIGHT_GUI,
};
static void key_emit(unsigned code, int release)
{
	if (!code)
		return;
	if (code >> SET1_PREFIX_SHIFT)
		keyboard_input_byte(code >> SET1_PREFIX_SHIFT);
	keyboard_input_byte((code & SET1_CODE_MASK) |
			    (release ? SET1_RELEASE_BIT : 0));
}
static int contains(const struct hid_boot_keyboard_report *report,
		    unsigned usage)
{
	for (unsigned i = 0; i < HID_BOOT_KEY_SLOTS; i++)
		if (report->keys[i] == usage)
			return 1;
	return 0;
}
static void keyboard_report(struct usb_endpoint *ep, const unsigned char *data,
			    unsigned length)
{
	struct hid *h = ep->class_data;
	const struct hid_boot_keyboard_report *report = (const void *)data;
	if (length < sizeof(*report))
		return;
	/* Rollover reports do not describe which ordinary keys remain pressed. */
	for (unsigned i = 0; i < HID_BOOT_KEY_SLOTS; i++)
		if (report->keys[i] &&
		    report->keys[i] <= HID_USAGE_ERROR_UNDEFINED)
			return;
	for (unsigned i = 0; i < HID_KEYBOARD_MODIFIERS; i++)
		if ((report->modifiers ^ h->previous.modifiers) & (1u << i))
			key_emit(modifiers[i],
				 !(report->modifiers & (1u << i)));
	for (unsigned i = 0; i < HID_BOOT_KEY_SLOTS; i++) {
		unsigned key = h->previous.keys[i];
		if (key > HID_USAGE_ERROR_UNDEFINED && !contains(report, key)) {
			key_emit(keys[key], 1);
			if (h->repeat == key)
				h->repeat = 0;
		}
	}
	for (unsigned i = 0; i < HID_BOOT_KEY_SLOTS; i++) {
		unsigned key = report->keys[i];
		if (key > HID_USAGE_ERROR_UNDEFINED &&
		    !contains(&h->previous, key)) {
			key_emit(keys[key], 0);
			if (keys[key] && key != HID_USAGE_CAPS_LOCK &&
			    key != HID_USAGE_NUM_LOCK &&
			    key != HID_USAGE_SCROLL_LOCK) {
				h->repeat = key;
				h->deadline = time_deadline_ms(
					HID_KEY_REPEAT_DELAY_MS);
			}
		}
	}
	h->previous = *report;
}
static void mouse_report(struct usb_endpoint *ep, const unsigned char *data,
			 unsigned length)
{
	(void)ep;
	const struct hid_extended_mouse_report *report = (const void *)data;
	if (length >= sizeof(report->boot))
		mouse_input_report(
			report->boot.x, report->boot.y,
			length >= sizeof(*report) ? report->wheel : 0,
			report->boot.buttons & HID_MOUSE_BUTTON_MASK);
}
static void hid_tick(struct usb_endpoint *ep)
{
	struct hid *h = ep->class_data;
	if (h && h->interface->protocol == HID_PROTOCOL_KEYBOARD && h->repeat &&
	    time_now_ms() >= h->deadline) {
		key_emit(keys[h->repeat], 0);
		h->deadline = time_deadline_ms(HID_KEY_REPEAT_INTERVAL_MS);
	}
}
int usb_hid_set_leds(unsigned leds)
{
	int result = -ENODEV;
	/* Console LED bits are Scroll/Num/Caps; HID bits are Num/Caps/Scroll. */
	unsigned char report =
		(leds & CONSOLE_LED_SCROLL ? HID_LED_SCROLL : 0) |
		(leds & CONSOLE_LED_NUM ? HID_LED_NUM : 0) |
		(leds & CONSOLE_LED_CAPS ? HID_LED_CAPS : 0);
	mutex_lock(&keyboards_lock);
	keyboard_leds = report;
	for (struct hid *h = keyboards; h; h = h->next) {
		struct usb_device *u = h->interface->usb;
		if (!u->online)
			continue;
		mutex_lock(&u->lock);
		int ret = usb_control(
			u, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
			HID_REQ_SET_REPORT,
			HID_REPORT_VALUE(HID_REPORT_OUTPUT, 0),
			h->interface->number, &report, 1);
		mutex_unlock(&u->lock);
		if (ret >= 0)
			result = 0;
		else if (result == -ENODEV)
			result = ret;
	}
	mutex_unlock(&keyboards_lock);
	return result;
}
static void hid_disconnect(struct usb_interface *interface)
{
	struct hid *h = interface->class_data;
	if (!h)
		return;
	mutex_lock(&keyboards_lock);
	if (interface->protocol == HID_PROTOCOL_KEYBOARD) {
		struct hid **link = &keyboards;
		while (*link && *link != h)
			link = &(*link)->next;
		if (*link)
			*link = h->next;
		struct hid_boot_keyboard_report empty = { 0 };
		keyboard_report(h->in, (const unsigned char *)&empty,
				sizeof(empty));
	} else
		mouse_input_report(0, 0, 0, 0);
	h->in->receive = NULL;
	h->in->tick = NULL;
	h->in->class_data = NULL;
	interface->class_data = NULL;
	mutex_unlock(&keyboards_lock);
	free(h);
}
static int hid_probe(struct usb_interface *interface)
{
	if (interface->protocol != HID_PROTOCOL_KEYBOARD &&
	    interface->protocol != HID_PROTOCOL_MOUSE)
		return -EOPNOTSUPP;
	struct usb_endpoint *in = NULL;
	for (unsigned i = 0; i < interface->endpoint_count; i++) {
		struct usb_endpoint *e = interface->endpoints[i];
		if ((e->address & USB_DIR_IN) &&
		    (e->attributes & USB_ENDPOINT_TYPE_MASK) ==
			    USB_XFER_INTERRUPT) {
			in = e;
			break;
		}
	}
	if (!in ||
	    in->max_packet < (interface->protocol == HID_PROTOCOL_KEYBOARD ?
				      sizeof(struct hid_boot_keyboard_report) :
				      sizeof(struct hid_boot_mouse_report)))
		return -EINVAL;
	struct hid *h = zalloc(sizeof(*h));
	if (!h)
		return -ENOMEM;
	struct usb_device *u = interface->usb;
	int ret = usb_control(
		u, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
		HID_REQ_SET_PROTOCOL, HID_PROTOCOL_BOOT, interface->number,
		NULL, 0);
	if (ret < 0) {
		free(h);
		return ret;
	}
	/* Report changes are delivered immediately; typematic repeats are local. */
	ret = usb_control(u, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
			  HID_REQ_SET_IDLE, 0, interface->number, NULL, 0);
	if (ret < 0 && ret != -EPIPE) {
		free(h);
		return ret;
	}
	ret = interface->protocol == HID_PROTOCOL_KEYBOARD ?
		      keyboard_input_init() :
		      mouse_input_init();
	if (ret < 0) {
		free(h);
		return ret;
	}
	h->interface = interface;
	h->in = in;
	in->class_data = h;
	in->tick = hid_tick;
	interface->class_data = h;
	ret = usb_interrupt_start(u, in,
				  interface->protocol == HID_PROTOCOL_KEYBOARD ?
					  keyboard_report :
					  mouse_report);
	if (ret < 0) {
		interface->class_data = NULL;
		in->class_data = NULL;
		in->tick = NULL;
		free(h);
		return ret;
	}
	if (interface->protocol == HID_PROTOCOL_KEYBOARD) {
		keyboard_input_start();
		mutex_lock(&keyboards_lock);
		h->next = keyboards;
		keyboards = h;
		unsigned char leds = keyboard_leds;
		mutex_unlock(&keyboards_lock);
		usb_control(u,
			    USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
			    HID_REQ_SET_REPORT,
			    HID_REPORT_VALUE(HID_REPORT_OUTPUT, 0),
			    interface->number, &leds, sizeof(leds));
	}
	return 0;
}
static driver_t keyboard_driver = { .name = "usb-hid-keyboard",
				    .bus = DEVICE_BUS_USB,
				    .usb_class = USB_CLASS_HID,
				    .usb_subclass = HID_SUBCLASS_BOOT,
				    .usb_protocol = HID_PROTOCOL_KEYBOARD,
				    .probe_usb = hid_probe,
				    .disconnect_usb = hid_disconnect };
static driver_t mouse_driver = { .name = "usb-hid-mouse",
				 .bus = DEVICE_BUS_USB,
				 .usb_class = USB_CLASS_HID,
				 .usb_subclass = HID_SUBCLASS_BOOT,
				 .usb_protocol = HID_PROTOCOL_MOUSE,
				 .probe_usb = hid_probe,
				 .disconnect_usb = hid_disconnect };
DRIVER_REGISTER(keyboard_driver);
DRIVER_REGISTER(mouse_driver);
