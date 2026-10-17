/* xHCI topology transport with queued isochronous service intervals. */
#include <device/usb/usb.h>
#include <fs/sysfs.h>
#include "xhci.h"
#include <lib/klib.h>
#include <ps/ps.h>
#include <mm/mmu.h>
#include <macro.h>
#include <errno.h>
#include <int/int.h>
#include <int/dsr.h>

#define XHCI_SLOTS 127
#define XHCI_HOSTS 4
#define RING_TRBS 2048
#define RING_SEGMENT_TRBS 256
#define RING_PAGES (RING_TRBS / RING_SEGMENT_TRBS)
#define RING_BYTES (RING_PAGES * PAGE_SIZE)
struct trb {
	uint64_t pointer;
	uint32_t status, control;
};
struct ring {
	struct trb *trbs;
	unsigned next, cycle;
};
struct iso_request {
	unsigned count, completed, done;
	uint64_t trbs[USB_ISO_PACKETS][2], deadline;
	struct usb_iso_packet *packets;
	void *data, *context;
	usb_iso_complete complete;
};
struct endpoint {
	struct ring ring;
	void *buffer;
	unsigned pending, done, code, actual, requested, dci;
	uint64_t last;
	struct usb_endpoint *usb;
	unsigned buffer_pages;
	struct iso_request iso[2];
	unsigned iso_frame, iso_frame_valid;
};
struct slot {
	struct usb_device usb;
	void *input, *output;
	struct endpoint ep[XHCI_ENDPOINTS];
	unsigned detached;
};
struct xhci {
	volatile unsigned char *base, *op, *runtime, *doorbell;
	unsigned index, ports, slots, context_size, alive;
	struct ring commands;
	struct trb *events;
	unsigned event_index, event_cycle;
	uint64_t *dcbaa;
	struct xhci_erst_entry *erst;
	unsigned command_done, command_code, command_slot;
	uint64_t command_pointer;
	struct slot *devices[XHCI_SLOTS + 1];
	unsigned port_slot[XHCI_MAX_PORTS + 1];
	uint64_t next_port_poll;
	unsigned ports_changed, irq_enabled, irq_queued;
	mutex_t lock;
};
static struct xhci *hosts[XHCI_HOSTS];
static unsigned host_count;

static void iso_finish_request(struct usb_device *, struct endpoint *, unsigned,
			       int);
static unsigned rd(volatile unsigned char *p, unsigned off)
{
	return *(volatile uint32_t *)(p + off);
}
static void wr(volatile unsigned char *p, unsigned off, unsigned value)
{
	*(volatile uint32_t *)(p + off) = value;
}
static void wr64(volatile unsigned char *p, unsigned off, uint64_t value)
{
	wr(p, off, value);
	wr(p, off + sizeof(uint32_t), value >> (sizeof(uint32_t) * 8));
}
static void *dma_page(void)
{
	void *p = (void *)vm_alloc_dma(1);
	if (p)
		memset(p, 0, PAGE_SIZE);
	return p;
}
static int ring_init(struct ring *r)
{
	r->trbs = (void *)vm_alloc_dma(RING_PAGES);
	if (r->trbs)
		memset(r->trbs, 0, RING_BYTES);
	r->cycle = 1;
	return r->trbs ? 0 : -ENOMEM;
}
static uint64_t ring_put(struct ring *r, uint64_t pointer, unsigned status,
			 unsigned control)
{
	struct trb *t = &r->trbs[r->next];
	uint64_t address = VIRT_TO_PHY(t);
	t->pointer = pointer;
	t->status = status;
	BARRIER();
	t->control = control | r->cycle;
	BARRIER();
	if (++r->next % RING_SEGMENT_TRBS == RING_SEGMENT_TRBS - 1) {
		t = &r->trbs[r->next];
		unsigned next = (r->next + 1) % RING_TRBS;
		t->pointer = VIRT_TO_PHY(&r->trbs[next]);
		t->status = 0;
		BARRIER();
		t->control = XHCI_TRB_TYPE(XHCI_TRB_LINK) |
			     (!next ? XHCI_TRB_TOGGLE_CYCLE : 0) |
			     (control & XHCI_TRB_CHAIN) | r->cycle;
		r->next = next;
		if (!next)
			r->cycle ^= 1;
	}
	return address;
}
static void stop_host(struct xhci *h)
{
	h->alive = 0;
	wr(h->runtime, XHCI_IMAN, 0);
	for (unsigned sid = 1; sid <= h->slots; sid++)
		if (h->devices[sid])
			h->devices[sid]->usb.online = 0;
	wr(h->op, XHCI_USBCMD, rd(h->op, XHCI_USBCMD) & ~XHCI_CMD_RUN);
	printk("usb: xhci%u stopped after controller timeout\n", h->index);
	/* Retain DMA mappings: a controller that fails to halt may still DMA. */
}
static int wait_bits(volatile unsigned char *p, unsigned off, unsigned mask,
		     unsigned value, unsigned ms)
{
	uint64_t deadline = time_deadline_ms(ms);
	do {
		if ((rd(p, off) & mask) == value)
			return 0;
		time_wait(1);
	} while (time_now_ms() < deadline);
	return -ETIMEDOUT;
}
static void events_poll(struct xhci *h)
{
	unsigned budget = RING_TRBS;
	while (budget--) {
		volatile struct trb *t = &h->events[h->event_index];
		unsigned control = t->control, type;
		if ((control & XHCI_TRB_CYCLE) != h->event_cycle)
			break;
		BARRIER();
		type = xhci_event_type(control);
		if (type == XHCI_EVENT_COMMAND &&
		    t->pointer == h->command_pointer) {
			h->command_code = xhci_completion_code(t->status);
			h->command_slot = xhci_event_slot(control);
			h->command_done = 1;
		} else if (type == XHCI_EVENT_PORT_CHANGE) {
			h->ports_changed = 1;
		} else if (type == XHCI_EVENT_TRANSFER) {
			unsigned sid = xhci_event_slot(control),
				 dci = xhci_event_endpoint(control);
			struct slot *s = sid <= h->slots ? h->devices[sid] :
							   NULL;
			if (s && dci && s->ep[dci].pending) {
				struct endpoint *ep = &s->ep[dci];
				uint64_t ring_begin =
					VIRT_TO_PHY(ep->ring.trbs);
				if (t->pointer >= ring_begin &&
				    t->pointer < ring_begin + RING_BYTES) {
					unsigned code =
						xhci_completion_code(t->status);
					/* Empty isochronous rings report endpoint state, not TD completion. */
					if (code == XHCI_CC_RING_UNDERRUN ||
					    code == XHCI_CC_RING_OVERRUN)
						goto next_event;
					unsigned residual =
						t->status &
						XHCI_TRB_RESIDUAL_MASK;
					if (ep->iso[0].count ||
					    ep->iso[1].count) {
						for (unsigned bank = 0;
						     bank < 2; bank++) {
							struct iso_request *r =
								&ep->iso[bank];
							for (unsigned i = 0;
							     i < r->count;
							     i++) {
								struct usb_iso_packet *p =
									&r->packets
										 [i];
								if (p->status !=
									    -EINPROGRESS ||
								    (t->pointer !=
									     r->trbs[i]
										    [0] &&
								     t->pointer !=
									     r->trbs[i]
										    [1]))
									continue;
								p->actual =
									(control &
									 XHCI_EVENT_HAS_DATA) ?
										(residual <= p->length ?
											 residual :
											 0) :
									residual <= p->length ?
										p->length -
											residual :
										0;
								__atomic_store_n(
									&p->status,
									(code == XHCI_CC_SUCCESS ||
									 code == XHCI_CC_SHORT_PACKET) ?
										0 :
									code == 23 ?
										-EXDEV :
									code == 3 ?
										-EOVERFLOW :
										-EIO,
									__ATOMIC_RELEASE);
								if (ep->usb &&
								    ep->usb->tick)
									ep->usb->tick(
										ep->usb);
								if (++r->completed ==
								    r->count) {
									r->done =
										1;
									iso_finish_request(
										&s->usb,
										ep,
										bank,
										0);
								}
								goto next_event;
							}
						}
						goto next_event;
					}
					if (dci != XHCI_DCI_CONTROL ||
					    code == XHCI_CC_SHORT_PACKET)
						ep->actual =
							residual <= ep->requested ?
								ep->requested -
									residual :
								0;
					/* A short control data stage is followed by its status stage. */
					if (dci != XHCI_DCI_CONTROL ||
					    t->pointer == ep->last ||
					    (code != XHCI_CC_SUCCESS &&
					     code != XHCI_CC_SHORT_PACKET)) {
						ep->code = code;
						ep->done = 1;
					}
				}
			}
		}
next_event:
		if (++h->event_index == RING_TRBS) {
			h->event_index = 0;
			h->event_cycle ^= 1;
		}
		wr64(h->runtime, XHCI_ERDP,
		     VIRT_TO_PHY(&h->events[h->event_index]) | XHCI_ERDP_BUSY);
	}
}
static void xhci_deferred(void *context)
{
	struct xhci *h = context;
	mutex_lock(&h->lock);
	if (h->alive)
		events_poll(h);
	__atomic_store_n(&h->irq_queued, 0, __ATOMIC_RELEASE);
	mutex_unlock(&h->lock);
}
static void xhci_irq(intr_frame *frame)
{
	(void)frame;
	for (unsigned i = 0; i < host_count; i++) {
		struct xhci *h = hosts[i];
		if (!h || !h->alive || !h->irq_enabled ||
		    !(rd(h->runtime, XHCI_IMAN) & 1))
			continue;
		wr(h->runtime, XHCI_IMAN, 3);
		wr(h->op, XHCI_USBSTS, 1u << 3);
		if (!__atomic_exchange_n(&h->irq_queued, 1, __ATOMIC_ACQ_REL) &&
		    !dsr_add(xhci_deferred, h))
			__atomic_store_n(&h->irq_queued, 0, __ATOMIC_RELEASE);
	}
}
static int command(struct xhci *h, unsigned type, uint64_t pointer,
		   unsigned flags)
{
	if (!h->alive)
		return -ENODEV;
	h->command_done = 0;
	h->command_pointer =
		ring_put(&h->commands, pointer, 0, XHCI_TRB_TYPE(type) | flags);
	wr(h->doorbell, 0, 0);
	uint64_t deadline = time_deadline_ms(XHCI_COMMAND_TIMEOUT_MS);
	while (!h->command_done && time_now_ms() < deadline) {
		events_poll(h);
		if (!h->command_done)
			time_wait(1);
	}
	if (!h->command_done) {
		stop_host(h);
		return -ETIMEDOUT;
	}
	if (h->command_code != XHCI_CC_SUCCESS) {
		printk("usb: xhci command %u completion %u\n", type,
		       h->command_code);
		return -EIO;
	}
	return h->command_slot;
}
static int endpoint_init(struct slot *s, unsigned dci, unsigned pages)
{
	struct endpoint *ep = &s->ep[dci];
	if (ep->buffer && ep->buffer_pages >= pages)
		return 0;
	if (!ep->ring.trbs && ring_init(&ep->ring))
		return -ENOMEM;
	void *buffer = (void *)vm_alloc_dma(pages);
	if (!buffer)
		return -ENOMEM;
	if (ep->buffer)
		vm_free((vaddr_t)ep->buffer, ep->buffer_pages);
	ep->buffer = buffer;
	ep->buffer_pages = pages;
	memset(ep->buffer, 0, pages * PAGE_SIZE);
	ep->dci = dci;
	return ep->buffer ? 0 : -ENOMEM;
}
static unsigned *context(struct xhci *h, void *p, unsigned index)
{
	return (unsigned *)((char *)p + index * h->context_size);
}
static void input_copy(struct xhci *h, struct slot *s)
{
	memset(s->input, 0, PAGE_SIZE);
	memcpy((char *)s->input + h->context_size, s->output,
	       XHCI_ENDPOINTS * h->context_size);
}
static int endpoint_reset(struct xhci *h, struct slot *s, unsigned dci)
{
	struct endpoint *ep = &s->ep[dci];
	struct xhci_endpoint_context *output =
		(void *)context(h, s->output, dci);
	unsigned state = output->state_interval & XHCI_EP_STATE_MASK;
	int ret = 0;
	if (state == XHCI_EP_HALTED)
		ret = command(h, XHCI_CMD_RESET_ENDPOINT, 0,
			      XHCI_TRB_SLOT(s->usb.slot) |
				      XHCI_TRB_ENDPOINT(dci));
	else if (state == XHCI_EP_RUNNING)
		ret = command(h, XHCI_CMD_STOP_ENDPOINT, 0,
			      XHCI_TRB_SLOT(s->usb.slot) |
				      XHCI_TRB_ENDPOINT(dci));
	if (ret < 0)
		return ret;
	ep->pending = ep->done = 0;
	return command(h, XHCI_CMD_SET_DEQUEUE,
		       VIRT_TO_PHY(&ep->ring.trbs[ep->ring.next]) |
			       ep->ring.cycle,
		       XHCI_TRB_SLOT(s->usb.slot) | XHCI_TRB_ENDPOINT(dci));
}
/* Called with the host lock. A connection-change bit invalidates the old
 * generation even when a device was unplugged and reinserted between polls. */
static int device_attached(struct xhci *host, struct usb_device *device)
{
	if (!host->alive || !device->online ||
	    host->devices[device->slot] != (struct slot *)device)
		return 0;
	for (struct usb_device *p = device->parent; p; p = p->parent)
		if (!p->online)
			return 0;
	unsigned status = rd(host->op, xhci_port_offset(device->port));
	if (!(status & XHCI_PORT_CONNECTED) ||
	    (status & XHCI_PORT_CONNECT_CHANGE)) {
		device->online = 0;
		return 0;
	}
	return 1;
}
static int completion_wait(struct xhci *h, struct slot *s, struct endpoint *ep)
{
	uint64_t deadline = time_deadline_ms(XHCI_TRANSFER_TIMEOUT_MS);
	while (!ep->done && h->alive && time_now_ms() < deadline) {
		if (!device_attached(h, &s->usb)) {
			ep->pending = 0;
			return -ENODEV;
		}
		events_poll(h);
		if (!ep->done) {
			mutex_unlock(&h->lock);
			time_wait(1);
			mutex_lock(&h->lock);
		}
	}
	if (!device_attached(h, &s->usb)) {
		ep->pending = 0;
		return -ENODEV;
	}
	if (!ep->done) {
		printk("usb: transfer timeout slot=%u endpoint=%u\n",
		       s->usb.slot, ep->dci);
		endpoint_reset(h, s, ep->dci);
		return -ETIMEDOUT;
	}
	ep->pending = 0;
	if (ep->code == XHCI_CC_STALL)
		return -EPIPE;
	if (ep->code != XHCI_CC_SUCCESS && ep->code != XHCI_CC_SHORT_PACKET) {
		printk("usb: transfer slot=%u endpoint=%u completion=%u\n",
		       s->usb.slot, ep->dci, ep->code);
		endpoint_reset(h, s, ep->dci);
		return -EIO;
	}
	return ep->actual;
}
static int xhci_control(struct usb_device *u, unsigned type, unsigned request,
			unsigned value, unsigned index, void *data,
			unsigned length)
{
	struct xhci *h = u->host;
	if (length > USB_TRANSFER_MAX || (length && !data))
		return -EINVAL;
	mutex_lock(&u->control_lock);
	mutex_lock(&h->lock);
	if (!device_attached(h, u)) {
		mutex_unlock(&h->lock);
		mutex_unlock(&u->control_lock);
		return -ENODEV;
	}
	struct slot *s = h->devices[u->slot];
	struct endpoint *ep = &s->ep[XHCI_DCI_CONTROL];
	if (length && !(type & USB_DIR_IN))
		memcpy(ep->buffer, data, length);
	ep->requested = ep->actual = length;
	ep->done = 0;
	ep->pending = 1;
	struct usb_setup_packet setup = { .type = type,
					  .request = request,
					  .value = value,
					  .index = index,
					  .length = length };
	uint64_t immediate;
	memcpy(&immediate, &setup, sizeof(immediate));
	unsigned transfer_type = !length	     ? XHCI_SETUP_NO_DATA :
				 (type & USB_DIR_IN) ? XHCI_SETUP_DATA_IN :
						       XHCI_SETUP_DATA_OUT;
	ring_put(&ep->ring, immediate, sizeof(setup),
		 XHCI_TRB_TYPE(XHCI_TRB_SETUP) | XHCI_TRB_IMMEDIATE |
			 transfer_type);
	if (length)
		ring_put(&ep->ring, VIRT_TO_PHY(ep->buffer), length,
			 XHCI_TRB_TYPE(XHCI_TRB_DATA) | XHCI_TRB_SHORT_EVENT |
				 ((type & USB_DIR_IN) ? XHCI_TRB_DIR_IN : 0));
	ep->last = ring_put(&ep->ring, 0, 0,
			    XHCI_TRB_TYPE(XHCI_TRB_STATUS) | XHCI_TRB_IOC |
				    ((!length || !(type & USB_DIR_IN)) ?
					     XHCI_TRB_DIR_IN :
					     0));
	wr(h->doorbell, xhci_doorbell_offset(u->slot), XHCI_DCI_CONTROL);
	int ret = completion_wait(h, s, ep);
	if (ret >= 0 && length && (type & USB_DIR_IN))
		memcpy(data, ep->buffer, ret);
	if (ret == -EPIPE)
		endpoint_reset(h, s, XHCI_DCI_CONTROL);
	mutex_unlock(&h->lock);
	mutex_unlock(&u->control_lock);
	return ret;
}
static unsigned endpoint_id(unsigned address)
{
	return (address & USB_ENDPOINT_NUMBER_MASK) * 2 +
	       !!(address & USB_DIR_IN);
}
static void normal_submit(struct usb_device *u, struct endpoint *ep,
			  unsigned length)
{
	struct xhci *h = u->host;
	ep->requested = ep->actual = length;
	ep->done = 0;
	ep->pending = 1;
	ep->last = ring_put(&ep->ring, VIRT_TO_PHY(ep->buffer), length,
			    XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_IOC |
				    XHCI_TRB_SHORT_EVENT);
	wr(h->doorbell, xhci_doorbell_offset(u->slot), ep->dci);
}
static int xhci_transfer(struct usb_device *u, struct usb_endpoint *e,
			 void *buffer, unsigned length)
{
	struct xhci *h = u->host;
	struct endpoint *ep = e->private;
	int ret;
	if (length > USB_TRANSFER_MAX || !ep || (length && !buffer))
		return -EINVAL;
	mutex_lock(&h->lock);
	if (!device_attached(h, u))
		ret = -ENODEV;
	else if (ep->pending)
		ret = -EBUSY;
	else {
		if (!(e->address & USB_DIR_IN) && length)
			memcpy(ep->buffer, buffer, length);
		normal_submit(u, ep, length);
		ret = completion_wait(h, h->devices[u->slot], ep);
		if (ret > 0 && (e->address & USB_DIR_IN))
			memcpy(buffer, ep->buffer, ret);
	}
	mutex_unlock(&h->lock);
	return ret;
}
static int xhci_interrupt_start(struct usb_device *u, struct usb_endpoint *e,
				void (*receive)(struct usb_endpoint *,
						const unsigned char *,
						unsigned))
{
	struct xhci *h = u->host;
	struct endpoint *ep = e->private;
	if (!ep || !(e->address & USB_DIR_IN) ||
	    (e->attributes & USB_ENDPOINT_TYPE_MASK) != USB_XFER_INTERRUPT)
		return -EINVAL;
	mutex_lock(&h->lock);
	if (!device_attached(h, u)) {
		mutex_unlock(&h->lock);
		return -ENODEV;
	}
	if (!receive || ep->pending) {
		mutex_unlock(&h->lock);
		return -EBUSY;
	}
	e->receive = receive;
	normal_submit(u, ep,
		      e->bytes_interval ? e->bytes_interval :
					  e->max_packet * (e->burst + 1));
	mutex_unlock(&h->lock);
	return 0;
}
static int xhci_clear_halt(struct usb_device *u, struct usb_endpoint *e)
{
	int ret = usb_control(u, USB_DIR_OUT | USB_RECIP_ENDPOINT,
			      USB_REQ_CLEAR_FEATURE, USB_FEATURE_ENDPOINT_HALT,
			      e->address, NULL, 0);
	struct xhci *h = u->host;
	if (ret < 0)
		return ret;
	mutex_lock(&h->lock);
	ret = device_attached(h, u) ? endpoint_reset(h, h->devices[u->slot],
						     endpoint_id(e->address)) :
				      -ENODEV;
	mutex_unlock(&h->lock);
	return ret < 0 ? ret : 0;
}
static struct xhci_input_control *input_control(struct xhci *h, struct slot *s)
{
	return (void *)context(h, s->input, XHCI_INPUT_CONTROL_INDEX);
}
static struct xhci_slot_context *input_slot(struct xhci *h, struct slot *s)
{
	return (void *)context(h, s->input, XHCI_INPUT_SLOT_INDEX);
}
static struct xhci_endpoint_context *
input_endpoint(struct xhci *h, struct slot *s, unsigned dci)
{
	return (void *)context(h, s->input, XHCI_INPUT_EP_INDEX(dci));
}
static int xhci_update_ep0(struct usb_device *u, unsigned packet)
{
	struct xhci *h = u->host;
	struct slot *s = h->devices[u->slot];
	mutex_lock(&h->lock);
	input_copy(h, s);
	input_control(h, s)->add_flags = XHCI_CONTEXT_FLAG(XHCI_DCI_CONTROL);
	struct xhci_endpoint_context *ep =
		input_endpoint(h, s, XHCI_DCI_CONTROL);
	ep->type_packet = (ep->type_packet & ~XHCI_EP_PACKET_MASK) |
			  XHCI_EP_PACKET(packet);
	int ret = command(h, XHCI_CMD_EVALUATE_CONTEXT, VIRT_TO_PHY(s->input),
			  XHCI_TRB_SLOT(u->slot));
	mutex_unlock(&h->lock);
	return ret < 0 ? ret : 0;
}
static unsigned endpoint_interval(struct usb_device *u, struct usb_endpoint *e)
{
	if (u->speed >= USB_SPEED_HIGH)
		return e->interval > XHCI_PERIODIC_MAX_EXPONENT + 1 ?
			       XHCI_PERIODIC_MAX_EXPONENT :
			       e->interval - 1;
	if ((e->attributes & USB_ENDPOINT_TYPE_MASK) == USB_XFER_ISOCHRONOUS)
		return e->interval + 2;
	unsigned interval = XHCI_FULL_SPEED_MICROFRAME_EXPONENT;
	for (unsigned frames = e->interval; frames >>= 1;)
		interval++;
	return interval;
}
static unsigned endpoint_type(struct usb_endpoint *e)
{
	int input = e->address & USB_DIR_IN;
	if ((e->attributes & USB_ENDPOINT_TYPE_MASK) == USB_XFER_ISOCHRONOUS)
		return input ? XHCI_EP_ISOCH_IN : XHCI_EP_ISOCH_OUT;
	if ((e->attributes & USB_ENDPOINT_TYPE_MASK) == USB_XFER_BULK)
		return input ? XHCI_EP_BULK_IN : XHCI_EP_BULK_OUT;
	return input ? XHCI_EP_INTERRUPT_IN : XHCI_EP_INTERRUPT_OUT;
}
static int xhci_configure(struct usb_device *u)
{
	struct xhci *h = u->host;
	struct slot *s = h->devices[u->slot];
	unsigned highest = XHCI_DCI_CONTROL;
	int ret = 0;
	mutex_lock(&h->lock);
	input_copy(h, s);
	struct xhci_input_control *control = input_control(h, s);
	control->add_flags = XHCI_CONTEXT_FLAG(0); /* Slot context. */
	for (unsigned i = 0; i < u->endpoint_count; i++) {
		struct usb_endpoint *e = u->endpoints[i];
		unsigned dci = endpoint_id(e->address),
			 type = e->attributes & USB_ENDPOINT_TYPE_MASK;
		if (dci <= XHCI_DCI_CONTROL || dci >= XHCI_ENDPOINTS ||
		    (type != USB_XFER_BULK && type != USB_XFER_INTERRUPT &&
		     type != USB_XFER_ISOCHRONOUS) ||
		    !e->max_packet ||
		    e->max_packet > XHCI_MAX_ENDPOINT_PACKET ||
		    (s->ep[dci].usb && s->ep[dci].usb != e) ||
		    (type == USB_XFER_ISOCHRONOUS &&
		     u->speed == USB_SPEED_LOW) ||
		    ((type == USB_XFER_INTERRUPT ||
		      type == USB_XFER_ISOCHRONOUS) &&
		     (!e->interval ||
		      ((u->speed >= USB_SPEED_HIGH ||
			type == USB_XFER_ISOCHRONOUS) &&
		       e->interval > 16) ||
		      e->bytes_interval > e->max_packet * (e->burst + 1) *
						  (e->mult + 1)))) {
			ret = -EINVAL;
			break;
		}
		if (dci > highest)
			highest = dci;
		if (s->ep[dci].usb == e)
			continue;
		ret = endpoint_init(s, dci,
				    type == USB_XFER_ISOCHRONOUS ?
					    2 * USB_ISO_BUFFER_MAX / PAGE_SIZE :
				    type == USB_XFER_INTERRUPT ?
					    (e->max_packet * (e->burst + 1) +
					     PAGE_SIZE - 1) /
						    PAGE_SIZE :
					    1);
		if (ret)
			break;
		e->private = &s->ep[dci];
		s->ep[dci].usb = e;
		struct xhci_endpoint_context *ep = input_endpoint(h, s, dci);
		memset(ep, 0, h->context_size);
		unsigned interval =
			type != USB_XFER_BULK ? endpoint_interval(u, e) : 0;
		ep->state_interval = XHCI_EP_INTERVAL(interval) |
				     (e->mult << 8);
		ep->type_packet =
			(type == USB_XFER_ISOCHRONOUS ? 0 :
							XHCI_EP_ERROR_RETRIES) |
			XHCI_EP_TYPE(endpoint_type(e)) |
			XHCI_EP_BURST(e->burst) | XHCI_EP_PACKET(e->max_packet);
		ep->dequeue = VIRT_TO_PHY(s->ep[dci].ring.trbs) |
			      XHCI_TRB_CYCLE;
		ep->average_esit = e->max_packet;
		if (type != USB_XFER_BULK)
			ep->average_esit |= XHCI_EP_ESIT(
				e->bytes_interval ?
					e->bytes_interval :
					e->max_packet * (e->burst + 1));
		control->add_flags |= XHCI_CONTEXT_FLAG(dci);
		if (dci > highest)
			highest = dci;
	}
	if (!ret) {
		struct xhci_slot_context *sc = input_slot(h, s);
		sc->route_speed_entries =
			(sc->route_speed_entries & ~XHCI_SLOT_ENTRIES_MASK) |
			XHCI_SLOT_ENTRIES(highest);
		ret = command(h, XHCI_CMD_CONFIGURE_ENDPOINT,
			      VIRT_TO_PHY(s->input), XHCI_TRB_SLOT(u->slot));
	}
	mutex_unlock(&h->lock);
	return ret < 0 ? ret : 0;
}
static int xhci_interface_stop(struct usb_device *u,
			       struct usb_interface *interface)
{
	struct xhci *h = u->host;
	mutex_lock(&h->lock);
	if (!device_attached(h, u)) {
		mutex_unlock(&h->lock);
		return -ENODEV;
	}
	struct slot *s = h->devices[u->slot];
	for (unsigned i = 0; i < interface->endpoint_count; i++) {
		struct endpoint *ep = interface->endpoints[i]->private;
		if (ep && ep->pending) {
			mutex_unlock(&h->lock);
			return -EBUSY;
		}
	}
	for (unsigned i = 0; i < interface->endpoint_count; i++) {
		struct endpoint *ep = interface->endpoints[i]->private;
		if (!ep)
			continue;
		struct xhci_endpoint_context *output =
			(void *)context(h, s->output, ep->dci);
		if ((output->state_interval & XHCI_EP_STATE_MASK) ==
			    XHCI_EP_RUNNING &&
		    command(h, XHCI_CMD_STOP_ENDPOINT, 0,
			    XHCI_TRB_SLOT(u->slot) |
				    XHCI_TRB_ENDPOINT(ep->dci)) < 0) {
			mutex_unlock(&h->lock);
			return -EIO;
		}
	}
	input_copy(h, s);
	struct xhci_input_control *control = input_control(h, s);
	control->add_flags = XHCI_CONTEXT_FLAG(0);
	for (unsigned i = 0; i < interface->endpoint_count; i++) {
		struct endpoint *ep = interface->endpoints[i]->private;
		if (ep)
			control->drop_flags |= XHCI_CONTEXT_FLAG(ep->dci);
	}
	int ret = control->drop_flags ? command(h, XHCI_CMD_CONFIGURE_ENDPOINT,
						VIRT_TO_PHY(s->input),
						XHCI_TRB_SLOT(u->slot)) :
					0;
	if (ret >= 0) {
		events_poll(h);
		for (unsigned i = 0; i < interface->endpoint_count; i++) {
			struct usb_endpoint *e = interface->endpoints[i];
			struct endpoint *ep = e->private;
			if (!ep)
				continue;
			ep->usb = NULL;
			ep->pending = ep->done = 0;
			memset(ep->ring.trbs, 0, RING_BYTES);
			ep->ring.next = 0;
			ep->ring.cycle = 1;
			e->private = NULL;
		}
	}
	mutex_unlock(&h->lock);
	return ret < 0 ? ret : 0;
}
static void iso_finish_request(struct usb_device *u, struct endpoint *ep,
			       unsigned bank, int error)
{
	struct iso_request *r = &ep->iso[bank];
	usb_iso_complete complete = r->complete;
	void *context = r->context;
	if (!complete)
		return;
	for (unsigned i = 0; i < r->count; i++) {
		struct usb_iso_packet *p = &r->packets[i];
		if (p->status == -EINPROGRESS) {
			p->actual = 0;
			__atomic_store_n(&p->status, error ? error : -EIO,
					 __ATOMIC_RELEASE);
		}
		if (!p->status && (ep->usb->address & USB_DIR_IN) && p->actual)
			memcpy((char *)r->data + p->offset,
			       (char *)ep->buffer + bank * USB_ISO_BUFFER_MAX +
				       p->offset,
			       p->actual);
	}
	r->count = 0;
	r->complete = NULL;
	ep->pending = ep->iso[0].count || ep->iso[1].count;
	/* Completion callbacks publish state without issuing transport requests. */
	complete(context, error);
	ref_count_put(u);
}
static void iso_finish(struct usb_device *u, struct endpoint *ep, int error)
{
	for (unsigned bank = 0; bank < 2; bank++)
		iso_finish_request(u, ep, bank, error);
}
static int xhci_iso_submit(struct usb_device *u, struct usb_endpoint *e,
			   void *data, struct usb_iso_packet *packets,
			   unsigned count, usb_iso_complete complete,
			   void *context)
{
	if (!data || !packets || !complete || !count ||
	    count > USB_ISO_PACKETS ||
	    (e->attributes & USB_ENDPOINT_TYPE_MASK) != USB_XFER_ISOCHRONOUS)
		return -EINVAL;
	unsigned maximum = e->bytes_interval ? e->bytes_interval :
					       e->max_packet * (e->burst + 1);
	for (unsigned i = 0; i < count; i++)
		if (packets[i].offset > USB_ISO_BUFFER_MAX ||
		    packets[i].length >
			    USB_ISO_BUFFER_MAX - packets[i].offset ||
		    packets[i].length > maximum)
			return -EINVAL;
	struct xhci *h = u->host;
	mutex_lock(&h->lock);
	struct endpoint *ep = e->private;
	int ret = 0;
	if (!device_attached(h, u))
		ret = -ENODEV;
	else if (!ep || (ep->iso[0].count && ep->iso[1].count))
		ret = -EBUSY;
	if (ret < 0) {
		mutex_unlock(&h->lock);
		return ret;
	}
	unsigned scheduled = u->speed == USB_SPEED_FULL &&
			     !(e->address & USB_DIR_IN) && e->interval == 1;
	if (scheduled) {
		unsigned now = rd(h->runtime, 0) & 0x3fff;
		int delta = ((ep->iso_frame - now + 0x2000) & 0x3fff) - 0x2000;
		if (!ep->iso_frame_valid || delta < -128 ||
		    (!(ep->iso[0].count || ep->iso[1].count) && delta < 16)) {
			ep->iso_frame = (now + 23) & ~7u;
			ep->iso_frame_valid = 1;
		}
	}
	unsigned bank = ep->iso[0].count ? 1 : 0;
	struct iso_request *r = &ep->iso[bank];
	ref_count_get(u);
	r->data = data;
	r->packets = packets;
	r->count = count;
	r->completed = r->done = 0;
	r->context = context;
	r->complete = complete;
	ep->pending = 1;
	r->deadline = time_deadline_ms(
		3000 +
		((uint64_t)count * (1u << endpoint_interval(u, e)) + 7) / 8);
	for (unsigned i = 0; i < count; i++) {
		struct usb_iso_packet *p = &packets[i];
		p->actual = 0;
		p->status = -EINPROGRESS;
		if (!(e->address & USB_DIR_IN))
			memcpy((char *)ep->buffer + bank * USB_ISO_BUFFER_MAX +
				       p->offset,
			       (char *)data + p->offset, p->length);
		uint64_t address =
			VIRT_TO_PHY((char *)ep->buffer +
				    bank * USB_ISO_BUFFER_MAX + p->offset);
		unsigned first = p->length;
		unsigned boundary = 0x10000 - (address & 0xffff);
		if (first > boundary)
			first = boundary;
		unsigned flags = XHCI_TRB_TYPE(XHCI_TRB_ISOCH);
		if (scheduled) {
			/* Full-speed service intervals retain an explicit frame timeline. */
			flags |= ((ep->iso_frame >> 3) & 0x7ff) << 20;
			ep->iso_frame = (ep->iso_frame + 8) & 0x3fff;
		} else
			flags |= 1u << 31;
		if (u->speed >= USB_SPEED_SUPER && p->length) {
			unsigned n =
				(p->length + e->max_packet - 1) / e->max_packet;
			flags |= ((n - 1) / (e->burst + 1)) << 7;
			flags |= ((n - 1) % (e->burst + 1)) << 16;
		}
		r->trbs[i][0] = ring_put(&ep->ring, address, first,
					 flags | XHCI_TRB_CHAIN);
		r->trbs[i][1] = r->trbs[i][0];
		if (first < p->length)
			r->trbs[i][1] = ring_put(
				&ep->ring, address + first, p->length - first,
				XHCI_TRB_TYPE(XHCI_TRB_NORMAL) |
					XHCI_TRB_CHAIN);
		/* Event Data reports the total bytes transferred across all TD TRBs. */
		ring_put(&ep->ring, r->trbs[i][0], 0,
			 XHCI_TRB_TYPE(XHCI_TRB_EVENT_DATA) | XHCI_TRB_IOC);
	}
	wr(h->doorbell, xhci_doorbell_offset(u->slot), ep->dci);
	mutex_unlock(&h->lock);
	return 0;
}
static int xhci_child_attach(struct usb_device *, unsigned, unsigned);
static void xhci_child_detach(struct usb_device *, unsigned);
static int xhci_hub_configure(struct usb_device *u, unsigned ports,
			      unsigned think_time)
{
	struct xhci *h = u->host;
	mutex_lock(&h->lock);
	struct slot *s = h->devices[u->slot];
	input_copy(h, s);
	input_control(h, s)->add_flags = XHCI_CONTEXT_FLAG(0);
	struct xhci_slot_context *sc = input_slot(h, s);
	sc->route_speed_entries |=
		1u << 26; /* Hub, single transaction translator. */
	sc->port_latency = (sc->port_latency & 0x00ffffffu) | (ports << 24);
	if (u->speed == USB_SPEED_HIGH)
		sc->tt = (sc->tt & ~(3u << 16)) | (think_time << 16);
	int ret = command(h, XHCI_CMD_CONFIGURE_ENDPOINT, VIRT_TO_PHY(s->input),
			  XHCI_TRB_SLOT(u->slot));
	if (ret >= 0) {
		u->hub_ports = ports;
		char text[16];
		sprintf(text, "%u\n", ports);
		vfs_entry_remove(u->sysfs_entry, "maxchild");
		vfs_entry_text(u->sysfs_entry, "maxchild", text);
		u->hub_think_time = think_time;
	}
	mutex_unlock(&h->lock);
	return ret < 0 ? ret : 0;
}
static const struct usb_host_ops xhci_ops = {
	.control = xhci_control,
	.transfer = xhci_transfer,
	.interrupt_start = xhci_interrupt_start,
	.clear_halt = xhci_clear_halt,
	.configure = xhci_configure,
	.interface_stop = xhci_interface_stop,
	.iso_submit = xhci_iso_submit,
	.update_ep0 = xhci_update_ep0,
	.hub_configure = xhci_hub_configure,
	.child_attach = xhci_child_attach,
	.child_detach = xhci_child_detach,
};
static void slot_release(ref_count_t *reference)
{
	usb_device_cleanup((struct usb_device *)reference);
	free((struct slot *)reference);
}
static void slot_release_dma(struct slot *slot)
{
	for (unsigned i = 0; i < XHCI_ENDPOINTS; i++) {
		if (slot->ep[i].buffer)
			vm_free((vaddr_t)slot->ep[i].buffer,
				slot->ep[i].buffer_pages);
		if (slot->ep[i].ring.trbs)
			vm_free((vaddr_t)slot->ep[i].ring.trbs, RING_PAGES);
	}
	if (slot->input)
		vm_free((vaddr_t)slot->input, 1);
	if (slot->output)
		vm_free((vaddr_t)slot->output, 1);
}
/* xHCI 1.2 section 4.6.4 requires stopped or idle endpoints before Disable Slot. */
static int slot_stop_endpoints(struct xhci *host, struct slot *slot)
{
	if (!slot->output)
		return 0;
	for (unsigned dci = XHCI_DCI_CONTROL; dci < XHCI_ENDPOINTS; dci++) {
		struct xhci_endpoint_context *output =
			(void *)context(host, slot->output, dci);
		if ((output->state_interval & XHCI_EP_STATE_MASK) !=
		    XHCI_EP_RUNNING)
			continue;
		int result = command(host, XHCI_CMD_STOP_ENDPOINT, 0,
				     XHCI_TRB_SLOT(slot->usb.slot) |
					     XHCI_TRB_ENDPOINT(dci));
		if (result < 0)
			return result;
	}
	events_poll(host);
	return 0;
}
/* Disable Slot completes before DMA storage is reclaimed. Class teardown runs
 * outside the host lock so an in-flight class request can observe ENODEV. */
static void slot_detach(struct xhci *host, unsigned sid)
{
	struct slot *slot = sid ? host->devices[sid] : NULL;
	if (!slot || slot->detached)
		return;
	slot->usb.online = 0;
	slot->detached = 1;
	for (unsigned child = 1; child <= host->slots; child++)
		if (host->devices[child] &&
		    host->devices[child]->usb.parent == &slot->usb)
			slot_detach(host, child);
	int disabled = host->alive && !slot_stop_endpoints(host, slot) &&
		       command(host, XHCI_CMD_DISABLE_SLOT, 0,
			       XHCI_TRB_SLOT(sid)) >= 0;
	if (disabled) {
		events_poll(host);
		host->dcbaa[sid] = 0;
		host->devices[sid] = NULL;
		if (!slot->usb.parent)
			host->port_slot[slot->usb.port] = 0;
	}
	for (unsigned dci = 2; dci < XHCI_ENDPOINTS; dci++)
		iso_finish(&slot->usb, &slot->ep[dci], -ENODEV);
	mutex_unlock(&host->lock);
	usb_disconnect(&slot->usb);
	printk("usb: xhci%u port=%u slot=%u disconnected\n", host->index,
	       slot->usb.port, sid);
	if (disabled) {
		slot_release_dma(slot);
		ref_count_put(&slot->usb);
	}
	/* An unresponsive controller retains ownership of its DMA allocations. */
	mutex_lock(&host->lock);
}
static void port_detach(struct xhci *h, unsigned port)
{
	slot_detach(h, h->port_slot[port]);
}
static void xhci_child_detach(struct usb_device *parent, unsigned port)
{
	struct xhci *h = parent->host;
	mutex_lock(&h->lock);
	for (unsigned sid = 1; sid <= h->slots; sid++)
		if (h->devices[sid] && h->devices[sid]->usb.parent == parent &&
		    h->devices[sid]->usb.hub_port == port)
			slot_detach(h, sid);
	mutex_unlock(&h->lock);
}
static void port_acknowledge(struct xhci *host, unsigned port, unsigned status)
{
	wr(host->op, xhci_port_offset(port),
	   (status & XHCI_PORT_WRITABLE) | (status & XHCI_PORT_CHANGE));
}
static int device_attach(struct xhci *h, unsigned port, unsigned speed,
			 struct usb_device *parent, unsigned hub_port)
{
	int sid, ret;
	sid = command(h, XHCI_CMD_ENABLE_SLOT, 0, 0);
	if (sid <= 0 || (unsigned)sid > h->slots)
		return -ENODEV;
	struct slot *s = zalloc(sizeof(*s));
	if (!s) {
		command(h, XHCI_CMD_DISABLE_SLOT, 0, XHCI_TRB_SLOT(sid));
		return -ENOMEM;
	}
	ref_count_init(&s->usb.references, slot_release);
	h->devices[sid] = s;
	if (!parent)
		h->port_slot[port] = sid;
	s->usb.host = h;
	s->usb.busnum = h->index + 1;
	if (parent)
		sprintf(s->usb.path, "%s.%u", parent->path, hub_port);
	else
		sprintf(s->usb.path, "%u", port);
	s->usb.ops = &xhci_ops;
	s->usb.slot = sid;
	s->usb.port = port;
	s->usb.speed = speed;
	s->usb.parent = parent;
	s->usb.hub_port = hub_port;
	if (parent) {
		s->usb.depth = parent->depth + 1;
		s->usb.route = parent->route | ((hub_port < 15 ? hub_port : 15)
						<< (parent->depth * 4));
		if (speed == USB_SPEED_LOW || speed == USB_SPEED_FULL) {
			if (parent->speed == USB_SPEED_HIGH) {
				s->usb.tt_slot = parent->slot;
				s->usb.tt_port = hub_port;
			} else {
				s->usb.tt_slot = parent->tt_slot;
				s->usb.tt_port = parent->tt_port;
			}
		}
	}
	s->usb.online = 1;
	mutex_init(&s->usb.lock);
	mutex_init(&s->usb.control_lock);
	s->input = dma_page();
	s->output = dma_page();
	if (!s->input || !s->output || endpoint_init(s, XHCI_DCI_CONTROL, 1)) {
		ret = -ENOMEM;
		goto failed;
	}
	h->dcbaa[sid] = VIRT_TO_PHY(s->output);
	struct xhci_input_control *control = input_control(h, s);
	struct xhci_slot_context *sc = input_slot(h, s);
	struct xhci_endpoint_context *ep =
		input_endpoint(h, s, XHCI_DCI_CONTROL);
	control->add_flags = XHCI_CONTEXT_FLAG(0) |
			     XHCI_CONTEXT_FLAG(XHCI_DCI_CONTROL);
	sc->route_speed_entries = s->usb.route | XHCI_SLOT_SPEED(s->usb.speed) |
				  XHCI_SLOT_ENTRIES(XHCI_DCI_CONTROL);
	sc->port_latency = XHCI_SLOT_PORT(port);
	sc->tt = s->usb.tt_slot | (s->usb.tt_port << 8);
	unsigned packet = s->usb.speed >= USB_SPEED_SUPER ?
				  USB_EP0_PACKET_SUPER :
				  (s->usb.speed == USB_SPEED_HIGH ?
					   USB_EP0_PACKET_FULL_MAX :
					   USB_EP0_PACKET_LOW);
	ep->type_packet = XHCI_EP_ERROR_RETRIES |
			  XHCI_EP_TYPE(XHCI_EP_CONTROL) |
			  XHCI_EP_PACKET(packet);
	ep->dequeue = VIRT_TO_PHY(s->ep[XHCI_DCI_CONTROL].ring.trbs) |
		      XHCI_TRB_CYCLE;
	ep->average_esit = sizeof(struct usb_setup_packet);
	ret = command(h, XHCI_CMD_ADDRESS_DEVICE, VIRT_TO_PHY(s->input),
		      XHCI_TRB_SLOT(sid));
	if (ret < 0)
		goto failed;
	s->usb.address =
		((struct xhci_slot_context *)s->output)->address_state & 0xff;
	/* Class enumeration uses transport operations that acquire the host lock. */
	mutex_unlock(&h->lock);
	ret = usb_enumerate(&s->usb);
	mutex_lock(&h->lock);
	if (ret < 0)
		goto failed;
	if (!device_attached(h, &s->usb)) {
		ret = -ENODEV;
		goto failed;
	}
	printk("usb: xhci%u port=%u slot=%u speed=%u configured\n", h->index,
	       port, sid, s->usb.speed);
	return 0;
failed:
	slot_detach(h, sid);
	printk("usb: xhci%u port=%u enumeration failed: %d\n", h->index, port,
	       ret);
	return ret;
}
static int port_attach(struct xhci *h, unsigned port)
{
	unsigned off = xhci_port_offset(port), status = rd(h->op, off);
	int ret;
	if (!(status & XHCI_PORT_CONNECTED))
		return -ENODEV;
	port_acknowledge(h, port, status);
	time_wait(XHCI_PORT_DEBOUNCE_MS);
	status = rd(h->op, off);
	if (!(status & XHCI_PORT_CONNECTED) ||
	    (status & XHCI_PORT_CONNECT_CHANGE))
		return -ENODEV;
	wr(h->op, off,
	   (status & XHCI_PORT_WRITABLE) | XHCI_PORT_POWER | XHCI_PORT_RESET);
	ret = wait_bits(h->op, off, XHCI_PORT_RESET, 0,
			XHCI_COMMAND_TIMEOUT_MS);
	status = rd(h->op, off);
	if (ret || !(status & XHCI_PORT_ENABLED))
		return -ENODEV;
	port_acknowledge(h, port, status);
	time_wait(XHCI_PORT_SETTLE_MS);
	return device_attach(h, port, xhci_port_speed(status), NULL, 0);
}
static int xhci_child_attach(struct usb_device *parent, unsigned port,
			     unsigned speed)
{
	struct xhci *h = parent->host;
	if (parent->depth >= 5 || !port || port > parent->hub_ports)
		return -EINVAL;
	mutex_lock(&h->lock);
	int ret = device_attached(h, parent) ?
			  device_attach(h, parent->port, speed, parent, port) :
			  -ENODEV;
	mutex_unlock(&h->lock);
	return ret;
}
static void ports_poll(struct xhci *host)
{
	host->ports_changed = 0;
	host->next_port_poll = time_deadline_ms(XHCI_PORT_POLL_MS);
	for (unsigned port = 1; port <= host->ports && host->alive; port++) {
		unsigned status = rd(host->op, xhci_port_offset(port));
		unsigned sid = host->port_slot[port];
		if (sid && (!(status & XHCI_PORT_CONNECTED) ||
			    (status & XHCI_PORT_CONNECT_CHANGE) ||
			    !host->devices[sid]->usb.online))
			port_detach(host, port);
		port_acknowledge(host, port, status);
		if (!host->port_slot[port] && (status & XHCI_PORT_CONNECTED))
			port_attach(host, port);
	}
	wr(host->op, XHCI_USBSTS, XHCI_STS_PORT_CHANGE);
}
static void poll_worker(void *arg)
{
	struct xhci *h = arg;
	for (;;) {
		mutex_lock(&h->lock);
		if (!h->alive) {
			for (unsigned port = 1; port <= h->ports; port++)
				port_detach(h, port);
			mutex_unlock(&h->lock);
			return;
		}
		events_poll(h);
		if (h->ports_changed || time_now_ms() >= h->next_port_poll)
			ports_poll(h);
		for (unsigned sid = 1; sid <= h->slots; sid++) {
			struct slot *s = h->devices[sid];
			if (!s || !s->usb.online)
				continue;
			for (unsigned dci = 2; dci < XHCI_ENDPOINTS; dci++) {
				struct endpoint *ep = &s->ep[dci];
				if (ep->iso[0].count || ep->iso[1].count) {
					for (unsigned bank = 0; bank < 2;
					     bank++) {
						struct iso_request *r =
							&ep->iso[bank];
						if (!r->count)
							continue;
						if (r->done)
							iso_finish_request(
								&s->usb, ep,
								bank, 0);
						else if (time_now_ms() >=
							 r->deadline) {
							endpoint_reset(h, s,
								       dci);
							iso_finish(&s->usb, ep,
								   -ETIMEDOUT);
							break;
						}
					}
					continue;
				}
				if (!ep->usb || !ep->usb->receive)
					continue;
				if (ep->usb->tick)
					ep->usb->tick(ep->usb);
				if (!ep->done)
					continue;
				ep->pending = ep->done = 0;
				if (ep->code == XHCI_CC_SUCCESS ||
				    ep->code == XHCI_CC_SHORT_PACKET) {
					ep->usb->receive(ep->usb, ep->buffer,
							 ep->actual);
					normal_submit(
						&s->usb, ep,
						ep->usb->bytes_interval ?
							ep->usb->bytes_interval :
							ep->usb->max_packet *
								(ep->usb->burst +
								 1));
				} else
					printk("usb: interrupt slot=%u endpoint=%u stopped completion=%u\n",
					       sid, dci, ep->code);
			}
		}
		mutex_unlock(&h->lock);
		/* Hub control requests and child enumeration acquire the host lock. */
		for (unsigned sid = 1; sid <= h->slots; sid++) {
			struct slot *s = h->devices[sid];
			if (s && s->usb.online)
				usb_hub_poll(&s->usb);
		}
		time_wait(XHCI_POLL_MS);
	}
}
static int xhci_probe(unsigned pci, uint16_t vendor, uint16_t device,
		      const pci_device_id *id)
{
	(void)vendor;
	(void)device;
	(void)id;
	if (pci_read_field(pci, PCI_PROG_IF, 1) != XHCI_PCI_PROGIF ||
	    host_count == XHCI_HOSTS)
		return -ENODEV;
	pci_resource r[7];
	pci_get_resources(pci, r);
	uint64_t end = r[0].start + r[0].size;
	if ((r[0].flags & XHCI_PCI_IO_RESOURCE) || r[0].size < PAGE_SIZE ||
	    r[0].start < DEVICE_IO_BEGIN || end > DEVICE_IO_END ||
	    end <= r[0].start)
		return -ENODEV;
	for (uint64_t page = r[0].start & PAGE_SIZE_MASK; page < end;
	     page += PAGE_SIZE) {
		if (mm_map_io(page) != 1)
			return -ENOMEM;
		mm_set_map_flag(page, PAGE_ENTRY_KERNEL_DATA | PAGE_ENTRY_CD |
					      PAGE_ENTRY_WT);
		arch_mm_invalidate(page);
	}
	struct xhci *h = zalloc(sizeof(*h));
	if (!h)
		return -ENOMEM;
	h->base = (void *)(uintptr_t)r[0].start;
	h->index = host_count;
	unsigned cap = rd(h->base, XHCI_CAP_HEADER),
		 params = rd(h->base, XHCI_HCSPARAMS1);
	h->op = h->base + xhci_cap_length(cap);
	unsigned dboff = xhci_doorbell_base(rd(h->base, XHCI_DBOFF)),
		 rtsoff = xhci_runtime_base(rd(h->base, XHCI_RTSOFF));
	h->ports = xhci_cap_ports(params);
	h->slots = xhci_cap_slots(params) < XHCI_SLOTS ?
			   xhci_cap_slots(params) :
			   XHCI_SLOTS;
	if (!h->slots || !h->ports ||
	    dboff + (h->slots + 1) * XHCI_DWORD_BYTES > r[0].size ||
	    rtsoff + XHCI_RUNTIME_MIN_SIZE > r[0].size ||
	    xhci_cap_length(cap) + XHCI_PORT_BASE +
			    h->ports * XHCI_PORT_STRIDE >
		    r[0].size) {
		free(h);
		return -EINVAL;
	}
	h->doorbell = h->base + dboff;
	h->runtime = h->base + rtsoff;
	h->context_size = rd(h->base, XHCI_HCCPARAMS1) & XHCI_CONTEXT_64 ?
				  XHCI_CONTEXT_LARGE_SIZE :
				  XHCI_CONTEXT_SMALL_SIZE;
	mutex_init(&h->lock);
	pci_write_field(pci, PCI_COMMAND, 2,
			pci_read_field(pci, PCI_COMMAND, 2) |
				XHCI_PCI_MEMORY_BUSMASTER);
	/* Release firmware ownership through the USB Legacy Support capability. */
	unsigned ext = xhci_extended_cap_offset(rd(h->base, XHCI_HCCPARAMS1)),
		 budget = XHCI_MAX_CAPABILITIES;
	while (ext && budget--) {
		if (ext + 2 * XHCI_DWORD_BYTES > r[0].size)
			return -EINVAL;
		unsigned legacy = rd(h->base, ext);
		if (xhci_extended_cap_id(legacy) == XHCI_LEGACY_CAP_ID) {
			wr(h->base, ext, legacy | XHCI_LEGACY_OS_OWNED);
			if (wait_bits(h->base, ext, XHCI_LEGACY_BIOS_OWNED, 0,
				      XHCI_COMMAND_TIMEOUT_MS))
				return -ETIMEDOUT;
			wr(h->base, ext + XHCI_DWORD_BYTES, 0);
		}
		unsigned next = xhci_extended_cap_next(legacy);
		if (!next)
			break;
		ext += next;
	}
	wr(h->op, XHCI_USBCMD, rd(h->op, XHCI_USBCMD) & ~XHCI_CMD_RUN);
	if (wait_bits(h->op, XHCI_USBSTS, XHCI_STS_HALTED, XHCI_STS_HALTED,
		      XHCI_COMMAND_TIMEOUT_MS))
		return -ETIMEDOUT;
	wr(h->op, XHCI_USBCMD, XHCI_CMD_RESET);
	if (wait_bits(h->op, XHCI_USBCMD, XHCI_CMD_RESET, 0,
		      XHCI_COMMAND_TIMEOUT_MS) ||
	    wait_bits(h->op, XHCI_USBSTS, XHCI_STS_NOT_READY, 0,
		      XHCI_COMMAND_TIMEOUT_MS))
		return -ETIMEDOUT;
	if (!(rd(h->op, XHCI_PAGESIZE) & XHCI_PAGE_4K))
		return -EOPNOTSUPP;
	h->dcbaa = dma_page();
	h->erst = dma_page();
	h->events = (void *)vm_alloc_dma(RING_PAGES);
	if (h->events)
		memset(h->events, 0, RING_BYTES);
	if (!h->dcbaa || !h->erst || !h->events || ring_init(&h->commands))
		return -ENOMEM;
	unsigned scratch = xhci_scratchpad_count(rd(h->base, XHCI_HCSPARAMS2));
	if (scratch) {
		if (scratch > PAGE_SIZE / sizeof(uint64_t))
			return -EOPNOTSUPP;
		uint64_t *array = dma_page();
		if (!array)
			return -ENOMEM;
		for (unsigned i = 0; i < scratch; i++) {
			void *page = dma_page();
			if (!page)
				return -ENOMEM;
			array[i] = VIRT_TO_PHY(page);
		}
		h->dcbaa[0] = VIRT_TO_PHY(array);
	}
	h->erst[0].address = VIRT_TO_PHY(h->events);
	h->erst[0].size = RING_TRBS;
	h->event_cycle = 1;
	wr64(h->op, XHCI_DCBAAP, VIRT_TO_PHY(h->dcbaa));
	wr64(h->op, XHCI_CRCR, VIRT_TO_PHY(h->commands.trbs) | XHCI_TRB_CYCLE);
	wr(h->runtime, XHCI_ERSTSZ, 1);
	wr64(h->runtime, XHCI_ERSTBA, VIRT_TO_PHY(h->erst));
	wr64(h->runtime, XHCI_ERDP, VIRT_TO_PHY(h->events));
	wr(h->runtime, XHCI_IMAN, 0);
	wr(h->op, XHCI_CONFIG, h->slots);
	h->alive = 1;
	wr(h->op, XHCI_USBCMD, XHCI_CMD_RUN);
	if (wait_bits(h->op, XHCI_USBSTS, XHCI_STS_HALTED, 0,
		      XHCI_COMMAND_TIMEOUT_MS)) {
		stop_host(h);
		return -ETIMEDOUT;
	}
	usb_inventory_host(h->index + 1, pci, h->ports);
	hosts[host_count++] = h;
	unsigned irq = pci_read_field(pci, PCI_INTERRUPT_LINE, 1);
	if (irq >= 3 && irq < 16 && !int_register_irq(irq, xhci_irq)) {
		unsigned command = pci_read_field(pci, PCI_COMMAND, 2);
		pci_write_field(pci, PCI_COMMAND, 2, command & ~(1u << 10));
		h->irq_enabled = 1;
		wr(h->runtime, XHCI_IMAN, 3);
		wr(h->op, XHCI_USBCMD, rd(h->op, XHCI_USBCMD) | (1u << 2));
		printk("usb: xhci%u interrupt line=%u\n", h->index, irq);
	}
	mutex_lock(&h->lock);
	for (unsigned p = 1; p <= h->ports && h->alive; p++) {
		unsigned off = xhci_port_offset(p), status = rd(h->op, off);
		if (!(status & XHCI_PORT_POWER)) {
			wr(h->op, off,
			   (status & XHCI_PORT_WRITABLE) | XHCI_PORT_POWER);
			time_wait(XHCI_PORT_SETTLE_MS);
		}
		if (rd(h->op, off) & XHCI_PORT_CONNECTED)
			port_attach(h, p);
	}
	mutex_unlock(&h->lock);
	return 0;
}
static void usb_workers_init(void)
{
	keyboard_input_start();
	for (unsigned i = 0; i < host_count; i++)
		ps_create(poll_worker, hosts[i], ps_normal, ps_kernel);
}
KERNEL_INIT(4, usb_workers_init);
static const pci_device_id xhci_ids[] = {
	{ PCI_ANY_ID, PCI_ANY_ID, XHCI_PCI_USB_CLASS, XHCI_PCI_CLASS_MASK }
};
static driver_t xhci_driver = { .name = "xhci",
				.bus = DEVICE_BUS_PCI,
				.pci_ids = xhci_ids,
				.pci_id_count = 1,
				.probe_pci = xhci_probe };
DRIVER_REGISTER(xhci_driver);
