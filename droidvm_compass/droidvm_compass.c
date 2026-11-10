// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DroidVM's magnetometer as an IIO device.
 *
 * The host's motion sensors are one virtio-input device (INPUT_PROP_ACCELEROMETER: acceleration
 * on ABS_X/Y/Z, angular velocity on ABS_RX/RY/RZ). With a magnetometer (crosvm
 * `--input motion[magnetometer=true]`) it also carries the magnetic field on ABS_HAT0X, ABS_HAT0Y
 * and ABS_HAT1X, 16 units per microtesla, and on ABS_HAT1Y the heading from magnetic north,
 * tilt-compensated, in hundredths of a degree: what an HID Compass 3D sensor reports.
 *
 * evdev has no magnetometer and Linux's readers of one are IIO's, so this binds to such a device as
 * an input handler and publishes it as an IIO magnetometer with the channels hid-sensor-magn-3d
 * has: in_magn_{x,y,z} in gauss and in_rot_from_north_magnetic_tilt_comp in degrees, buffered, with
 * a trigger named "<name>-dev<N>" the way iio-sensor-proxy finds a device's trigger. Its compass
 * (net.hadess.SensorProxy, `monitor-sensor --compass`) then works unchanged. The input device stays
 * what it is: iio-sensor-proxy still reads its acceleration through evdev.
 *
 * Each SYN_REPORT that changed the field or the heading commits the frame and fires the trigger
 * from a work item (the event arrives under the input core's spinlock).
 */

#include <linux/input.h>
#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/trigger.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#define DRV_NAME "droidvm-compass"

/* The device's axes, and its units (crosvm devices/src/virtio/input/defaults.rs). */
#define COMPASS_ABS_FIRST	ABS_HAT0X	/* x, y, z: HAT0X, HAT0Y, HAT1X */
#define COMPASS_ABS_HEADING	ABS_HAT1Y
#define COMPASS_UNITS_PER_UT	16
#define COMPASS_UNITS_PER_DEG	100

enum { SCAN_X, SCAN_Y, SCAN_Z, SCAN_HEADING, SCAN_TIMESTAMP };

struct compass {
	struct input_handle handle;
	struct iio_dev *indio;
	struct iio_trigger *trig;
	struct work_struct fire;

	spinlock_t lock;
	s32 pending[4];		/* this frame so far; event handler only */
	bool dirty;		/* event handler only */
	s32 latest[4];		/* the last whole frame; under lock */
	bool seen;		/* a frame has arrived; under lock */

	struct {
		s32 values[4];
		aligned_s64 timestamp;
	} scan;
};

#define COMPASS_MAGN(axis, index) {					\
	.type = IIO_MAGN,						\
	.modified = 1,							\
	.channel2 = IIO_MOD_##axis,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),		\
	.scan_index = (index),						\
	.scan_type = {							\
		.sign = 's',						\
		.realbits = 32,						\
		.storagebits = 32,					\
		.endianness = IIO_CPU,					\
	},								\
}

static const struct iio_chan_spec compass_channels[] = {
	COMPASS_MAGN(X, SCAN_X),
	COMPASS_MAGN(Y, SCAN_Y),
	COMPASS_MAGN(Z, SCAN_Z),
	{
		.type = IIO_ROT,
		.modified = 1,
		.channel2 = IIO_MOD_NORTH_MAGN_TILT_COMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) | BIT(IIO_CHAN_INFO_SCALE),
		.scan_index = SCAN_HEADING,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.endianness = IIO_CPU,
		},
	},
	IIO_CHAN_SOFT_TIMESTAMP(SCAN_TIMESTAMP),
};

static int compass_read_raw(struct iio_dev *indio, struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct compass *c = iio_priv(indio);
	unsigned long flags;
	bool seen;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		spin_lock_irqsave(&c->lock, flags);
		seen = c->seen;
		*val = c->latest[chan->scan_index];
		spin_unlock_irqrestore(&c->lock, flags);
		/* Nothing measured yet is not a zero field. */
		return seen ? IIO_VAL_INT : -EAGAIN;
	case IIO_CHAN_INFO_SCALE:
		if (chan->type == IIO_MAGN) {
			/* IIO's magnetic field is in gauss: 1 uT = 0.01 G. */
			*val = 1;
			*val2 = COMPASS_UNITS_PER_UT * 100;
			return IIO_VAL_FRACTIONAL;
		}
		*val = 1;
		*val2 = COMPASS_UNITS_PER_DEG;
		return IIO_VAL_FRACTIONAL;
	default:
		return -EINVAL;
	}
}

static const struct iio_info compass_info = {
	.read_raw = compass_read_raw,
};

static const struct iio_trigger_ops compass_trigger_ops = {};

static irqreturn_t compass_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio = pf->indio_dev;
	struct compass *c = iio_priv(indio);
	unsigned long flags;

	spin_lock_irqsave(&c->lock, flags);
	memcpy(c->scan.values, c->latest, sizeof(c->scan.values));
	spin_unlock_irqrestore(&c->lock, flags);
	iio_push_to_buffers_with_ts(indio, &c->scan, sizeof(c->scan), iio_get_time_ns(indio));
	iio_trigger_notify_done(indio->trig);
	return IRQ_HANDLED;
}

static void compass_fire(struct work_struct *work)
{
	struct compass *c = container_of(work, struct compass, fire);

	iio_trigger_poll_nested(c->trig);
}

static void compass_event(struct input_handle *handle, unsigned int type, unsigned int code,
			  int value)
{
	struct compass *c = handle->private;

	if (type == EV_ABS) {
		if (code >= COMPASS_ABS_FIRST && code < COMPASS_ABS_FIRST + 3) {
			c->pending[code - COMPASS_ABS_FIRST] = value;
			c->dirty = true;
		} else if (code == COMPASS_ABS_HEADING) {
			c->pending[SCAN_HEADING] = value;
			c->dirty = true;
		}
		return;
	}
	if (type != EV_SYN || code != SYN_REPORT || !c->dirty)
		return;
	/* Already under the input device's event_lock, with interrupts off. */
	spin_lock(&c->lock);
	memcpy(c->latest, c->pending, sizeof(c->latest));
	c->seen = true;
	spin_unlock(&c->lock);
	c->dirty = false;
	schedule_work(&c->fire);
}

static int compass_connect(struct input_handler *handler, struct input_dev *dev,
			   const struct input_device_id *id)
{
	struct iio_dev *indio;
	struct compass *c;
	int err;

	indio = iio_device_alloc(&dev->dev, sizeof(*c));
	if (!indio)
		return -ENOMEM;
	c = iio_priv(indio);
	c->indio = indio;
	spin_lock_init(&c->lock);
	INIT_WORK(&c->fire, compass_fire);

	indio->name = DRV_NAME;
	indio->info = &compass_info;
	indio->channels = compass_channels;
	indio->num_channels = ARRAY_SIZE(compass_channels);
	indio->modes = INDIO_DIRECT_MODE;

	err = iio_triggered_buffer_setup(indio, NULL, compass_trigger_handler, NULL);
	if (err)
		goto err_free;

	c->trig = iio_trigger_alloc(&dev->dev, "%s-dev%d", indio->name, iio_device_id(indio));
	if (!c->trig) {
		err = -ENOMEM;
		goto err_buffer;
	}
	c->trig->ops = &compass_trigger_ops;
	iio_trigger_set_drvdata(c->trig, c);
	err = iio_trigger_register(c->trig);
	if (err)
		goto err_trig_free;
	indio->trig = iio_trigger_get(c->trig);

	err = iio_device_register(indio);
	if (err)
		goto err_trig_put;

	c->handle.dev = dev;
	c->handle.handler = handler;
	c->handle.name = DRV_NAME;
	c->handle.private = c;
	err = input_register_handle(&c->handle);
	if (err)
		goto err_iio_unregister;
	err = input_open_device(&c->handle);
	if (err)
		goto err_handle;

	dev_info(&indio->dev, "magnetometer of %s\n", dev->name ?: "an input device");
	return 0;

err_handle:
	input_unregister_handle(&c->handle);
err_iio_unregister:
	iio_device_unregister(indio);
	/* indio->trig stays: iio_device_free drops it, as on disconnect. */
	goto err_trig_unregister;
err_trig_put:
	iio_trigger_put(indio->trig);
	indio->trig = NULL;
err_trig_unregister:
	iio_trigger_unregister(c->trig);
err_trig_free:
	iio_trigger_free(c->trig);
err_buffer:
	iio_triggered_buffer_cleanup(indio);
err_free:
	iio_device_free(indio);
	return err;
}

static void compass_disconnect(struct input_handle *handle)
{
	struct compass *c = handle->private;
	struct iio_dev *indio = c->indio;

	input_close_device(handle);
	input_unregister_handle(handle);
	cancel_work_sync(&c->fire);
	iio_device_unregister(indio);
	iio_trigger_unregister(c->trig);
	iio_trigger_free(c->trig);
	iio_triggered_buffer_cleanup(indio);
	iio_device_free(indio);
}

/* A motion-sensor device that carries a magnetometer: an accelerometer with the four hat axes. */
static const struct input_device_id compass_ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT | INPUT_DEVICE_ID_MATCH_ABSBIT |
			 INPUT_DEVICE_ID_MATCH_PROPBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_X)] = BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) | BIT_MASK(ABS_Z) |
			    BIT_MASK(ABS_HAT0X) | BIT_MASK(ABS_HAT0Y) | BIT_MASK(ABS_HAT1X) |
			    BIT_MASK(ABS_HAT1Y) },
		.propbit = { BIT_MASK(INPUT_PROP_ACCELEROMETER) },
	},
	{ },
};
MODULE_DEVICE_TABLE(input, compass_ids);

static struct input_handler compass_handler = {
	.event = compass_event,
	.connect = compass_connect,
	.disconnect = compass_disconnect,
	.name = DRV_NAME,
	.id_table = compass_ids,
};

static int __init compass_init(void)
{
	return input_register_handler(&compass_handler);
}

static void __exit compass_exit(void)
{
	input_unregister_handler(&compass_handler);
}

module_init(compass_init);
module_exit(compass_exit);

MODULE_DESCRIPTION("DroidVM magnetometer (virtio-input motion sensors) as an IIO compass");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("IIO_TRIGGERED_BUFFER");
