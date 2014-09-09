/*
 * Copyright (C) 2014 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "filter.h"

#ifdef UDEV_SYNC_SUPPORT
#include <libudev.h>
#endif

#ifdef __linux__

#ifdef UDEV_SYNC_SUPPORT
static int _udev_dev_is_fwraid(struct device *dev)
{
	const char *value;

	value = udev_device_get_property_value((struct udev_device *)dev->ext.handle, "ID_FS_TYPE");
	if (value && strcmp(value, "linux_raid_member") && strstr(value, "_raid_member"))
		return 1;

	return 0;
}
#else
static int _udev_dev_is_fwraid(struct device *dev)
{
	return 0;
}
#endif

static int _native_dev_is_fwraid(struct device *dev)
{
	log_verbose("%s: Firmware RAID detection is not supported by LVM natively. "
		    "Skipping firmware raid detection. ", dev_name(dev));
	return 0;
}

static int _dev_is_fwraid(struct device *dev)
{
	if (dev->ext.src == DEV_EXT_NONE)
		return _native_dev_is_fwraid(dev);

	if (dev->ext.src == DEV_EXT_UDEV)
		return _udev_dev_is_fwraid(dev);

	log_error(INTERNAL_ERROR "Missing hook for firmware RAID recognition "
		  "using external device info source %s", dev_ext_name(dev));

	return 0;
}

static int _ignore_fwraid(struct dev_filter *f __attribute__((unused)),
			   struct device *dev)
{
	int ret;

	if (!fwraid_filtering())
		return 1;

	ret = _dev_is_fwraid(dev);

	if (ret == 1) {
		log_debug_devs("%s: Skipping firmware RAID component device [%s:%p]",
				dev_name(dev), dev_ext_name(dev), dev->ext.handle);
		return 0;
	}

	if (ret < 0) {
		log_debug_devs("%s: Skipping: error in firmware RAID component detection",
			       dev_name(dev));
		return 0;
	}

	return 1;
}

static void _destroy(struct dev_filter *f)
{
	if (f->use_count)
		log_error(INTERNAL_ERROR "Destroying firmware RAID filter while in use %u times.", f->use_count);

	dm_free(f);
}

struct dev_filter *fwraid_filter_create(struct dev_types *dt __attribute__((unused)))
{
	struct dev_filter *f;

	if (!(f = dm_zalloc(sizeof(*f)))) {
		log_error("Firmware RAID filter allocation failed");
		return NULL;
	}

	f->passes_filter = _ignore_fwraid;
	f->destroy = _destroy;
	f->use_count = 0;
	f->private = NULL;

	log_debug_devs("Firmware RAID filter initialised.");

	return f;
}

#else

struct dev_filter *fwraid_filter_create(struct dev_types *dt __attribute__((unused)))
{
	return NULL;
}

#endif
