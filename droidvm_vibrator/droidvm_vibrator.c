// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DroidVM's vibrator as a force-feedback input device.
 *
 * The host's vibrator is a virtio-input device (crosvm `--input vibrator`, ids BUS_VIRTUAL
 * 4456:0001) that declares EV_FF/FF_RUMBLE and only listens: the guest tells it the rumble it
 * wants through the status queue, one event per change, EV_FF with code FF_RUMBLE and the strong
 * magnitude in the high 16 bits of the value, the weak in the low (both 0 stops), and the host plays
 * that on its motor.
 *
 * Linux's virtio_input passes only LEDs and sounds back to a device and gives it no force feedback,
 * so this binds to the vibrator as an input handler and registers "DroidVM Vibrator", an input
 * device with FF_RUMBLE through memless force feedback: programs upload and play rumble effects as
 * on any gamepad or phone motor (EVIOCSFF, then EV_FF), the input core times them, and each change
 * of the combined magnitude goes to the host by the virtio device's own status path.
 */

#include <linux/input.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#define DRV_NAME "droidvm-vibrator"

/* The host device's ids (crosvm devices/src/virtio/input/defaults.rs). */
#define VIBRATOR_BUSTYPE	BUS_VIRTUAL
#define VIBRATOR_VENDOR		0x4456
#define VIBRATOR_PRODUCT	0x0001
/* The rumble device's own: not the host device's, or this handler would bind to it as well. */
#define RUMBLE_PRODUCT		0x0002

struct vibrator {
	struct input_handle handle;
	struct input_dev *ff;
	struct work_struct add, remove;
	bool registered;	/* vibrator_wq only */

	spinlock_t lock;
	struct input_dev *host;	/* the host device; NULL once it has gone */
};

/*
 * The rumble device is registered and unregistered from here, never from connect or disconnect:
 * those run with the input core's lock held, which input_register_device and
 * input_unregister_device take themselves. Ordered, so a device's removal follows its addition.
 */
static struct workqueue_struct *vibrator_wq;

/*
 * memless calls this with the combined effect whenever it changes, under the FF device's
 * event_lock. The host device is reached through its own event callback (virtio_input's status
 * path, which queues the event under its own lock): input_inject_event would drop EV_FF, which
 * that device does not list among its capabilities.
 */
static int vibrator_play(struct input_dev *ff, void *data, struct ff_effect *effect)
{
	struct vibrator *v = data;
	unsigned long flags;
	u32 value;

	if (effect->type != FF_RUMBLE)
		return 0;
	value = (u32)effect->u.rumble.strong_magnitude << 16 | effect->u.rumble.weak_magnitude;
	spin_lock_irqsave(&v->lock, flags);
	if (v->host && v->host->event)
		v->host->event(v->host, EV_FF, FF_RUMBLE, (int)value);
	spin_unlock_irqrestore(&v->lock, flags);
	return 0;
}

static void vibrator_add(struct work_struct *work)
{
	struct vibrator *v = container_of(work, struct vibrator, add);
	int err = input_register_device(v->ff);

	if (err) {
		pr_warn(DRV_NAME ": cannot register the rumble device: %d\n", err);
		return;
	}
	v->registered = true;
	pr_info(DRV_NAME ": %s ready\n", v->ff->name);
}

static void vibrator_remove(struct work_struct *work)
{
	struct vibrator *v = container_of(work, struct vibrator, remove);

	/* A rumble still playing is the host's to end: it plays nothing longer than a few seconds. */
	if (v->registered)
		input_unregister_device(v->ff);
	else
		input_free_device(v->ff);
	kfree(v);
}

static int vibrator_connect(struct input_handler *handler, struct input_dev *dev,
			    const struct input_device_id *id)
{
	struct vibrator *v;
	struct input_dev *ff;
	int err;

	if (!dev->event)
		return -ENODEV;	/* nothing to send the rumble through */

	v = kzalloc(sizeof(*v), GFP_KERNEL);
	if (!v)
		return -ENOMEM;
	spin_lock_init(&v->lock);
	INIT_WORK(&v->add, vibrator_add);
	INIT_WORK(&v->remove, vibrator_remove);

	ff = input_allocate_device();
	if (!ff) {
		err = -ENOMEM;
		goto err_free;
	}
	ff->name = "DroidVM Vibrator";
	ff->phys = DRV_NAME "/input0";
	ff->id.bustype = BUS_VIRTUAL;
	ff->id.vendor = VIBRATOR_VENDOR;
	ff->id.product = RUMBLE_PRODUCT;
	ff->id.version = 1;
	ff->dev.parent = &dev->dev;
	input_set_capability(ff, EV_FF, FF_RUMBLE);
	err = input_ff_create_memless(ff, v, vibrator_play);
	if (err)
		goto err_ff_free;
	v->ff = ff;

	v->handle.dev = dev;
	v->handle.handler = handler;
	v->handle.name = DRV_NAME;
	v->handle.private = v;
	err = input_register_handle(&v->handle);
	if (err)
		goto err_ff_free;
	err = input_open_device(&v->handle);
	if (err)
		goto err_handle;

	v->host = dev;
	queue_work(vibrator_wq, &v->add);
	return 0;

err_handle:
	input_unregister_handle(&v->handle);
err_ff_free:
	input_free_device(ff);
err_free:
	kfree(v);
	return err;
}

static void vibrator_disconnect(struct input_handle *handle)
{
	struct vibrator *v = handle->private;
	unsigned long flags;

	/* No rumble reaches the host device from here on; one being sent finishes first. */
	spin_lock_irqsave(&v->lock, flags);
	v->host = NULL;
	spin_unlock_irqrestore(&v->lock, flags);
	input_close_device(handle);
	input_unregister_handle(handle);
	queue_work(vibrator_wq, &v->remove);
}

static const struct input_device_id vibrator_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_BUS | INPUT_DEVICE_ID_MATCH_VENDOR |
			 INPUT_DEVICE_ID_MATCH_PRODUCT,
		.bustype = VIBRATOR_BUSTYPE,
		.vendor = VIBRATOR_VENDOR,
		.product = VIBRATOR_PRODUCT,
	},
	{ },
};
MODULE_DEVICE_TABLE(input, vibrator_ids);

static struct input_handler vibrator_handler = {
	.connect = vibrator_connect,
	.disconnect = vibrator_disconnect,
	.name = DRV_NAME,
	.id_table = vibrator_ids,
};

static int __init vibrator_init(void)
{
	int err;

	vibrator_wq = alloc_ordered_workqueue(DRV_NAME, 0);
	if (!vibrator_wq)
		return -ENOMEM;
	err = input_register_handler(&vibrator_handler);
	if (err)
		destroy_workqueue(vibrator_wq);
	return err;
}

static void __exit vibrator_exit(void)
{
	input_unregister_handler(&vibrator_handler);
	destroy_workqueue(vibrator_wq);	/* runs the removals the disconnects queued */
}

module_init(vibrator_init);
module_exit(vibrator_exit);

MODULE_DESCRIPTION("DroidVM vibrator (virtio-input) as a force-feedback rumble device");
MODULE_LICENSE("GPL");
