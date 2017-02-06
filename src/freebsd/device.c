/*
 * Copyright (c) 2014 Jean-Sebastien Pedron <dumbbell@FreeBSD.org>
 * Copyright (c) 2016 Koop Mast <kwm@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/sysctl.h>

#include <paths.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/pciio.h>

#include "libdevq.h"

int
devq_device_get_devpath_from_fd(int fd,
    char *path, size_t *path_len)
{
	char tmp_path[256] = _PATH_DEV;
	size_t tmp_path_len;

	tmp_path_len = strlen(tmp_path);
	if (fdevname_r(fd, tmp_path + tmp_path_len,
	    sizeof(tmp_path) - tmp_path_len) == NULL)
		return (-1);

	tmp_path_len = strlen(tmp_path);
	if (path) {
		if (*path_len < tmp_path_len) {
			*path_len = tmp_path_len;
			errno = ENOMEM;
			return (-1);
		}

		memcpy(path, tmp_path, tmp_path_len);
	}
	if (path_len)
		*path_len = tmp_path_len;

	return (0);
}

static int
devq_compare_vgapci_busaddr(int i, int *domain, int *bus, int *slot,
    int *function)
{
	int ret;
	char sysctl_name[32], sysctl_value[128];
	size_t sysctl_value_len;

	sprintf(sysctl_name, "dev.vgapci.%d.%%location", i);

	sysctl_value_len = sizeof(sysctl_value);
	memset(sysctl_value, 0, sysctl_value_len);
	ret = sysctlbyname(sysctl_name, sysctl_value,
	    &sysctl_value_len, NULL, 0);
	if (ret != 0)
		return (-1);

	/*
	 * dev.vgapci.$m.%location can have two formats:
	 *     o  "pci0:2:0:0 handle=\_SB_.PCI0.PEG3.MXM3" (FreeBSD 11+)
	 *     o  "slot=1 function=0" (up-to FreeBSD 10)
	 */

	ret = sscanf(sysctl_value, "pci%d:%d:%d:%d %*s",
	    domain, bus, slot, function);
	if (ret == 4)
		return (0);

	ret = sscanf(sysctl_value, "slot=%d function=%d %*s",
	    slot, function);
	if (ret != 2)
		return (-1);

	sprintf(sysctl_name, "dev.vgapci.%d.%%parent", i);

	sysctl_value_len = sizeof(sysctl_value);
	memset(sysctl_value, 0, sysctl_value_len);
	ret = sysctlbyname(sysctl_name, sysctl_value,
	    &sysctl_value_len, NULL, 0);
	if (ret != 0)
		return (-1);

	ret = sscanf(sysctl_value, "pci%d", bus);
	if (ret != 1)
		return (-1);

	/* FIXME: What domain to assume? */
	*domain = 0;

	return (0);
}

int
devq_device_get_pcibusaddr(int fd, int *domain,
	int *bus, int *slot, int *function)
{
	int i, dev, ret;
	char sysctl_name[32], sysctl_value[128];
	const char *busid_format;
	size_t sysctl_value_len;

	/*
	 * FIXME: This function is specific to DRM devices.
	 */

	/*
	 * We don't need the driver name, but this function already
	 * walks the hw.dri.* tree and returns the number in
	 * hw.dri.$number.
	 */
	dev = devq_device_drm_get_drvname_from_fd(fd, NULL, NULL);
	if (dev < 0)
		return (-1);

	/*
	 * Read the hw.dri.$n.busid sysctl to get the location of the
	 * device on the PCI bus. We can then use this location to find
	 * the corresponding dev.vgapci.$m tree.
	 */
	sprintf(sysctl_name, "hw.dri.%d.busid", dev);

	busid_format = "pci:%d:%d:%d.%d";
	sysctl_value_len = sizeof(sysctl_value);
	memset(sysctl_value, 0, sysctl_value_len);
	ret = sysctlbyname(sysctl_name, sysctl_value, &sysctl_value_len,
	    NULL, 0);

	if (ret != 0) {
		/*
		 * If hw.dri.$n.busid isn't available, fallback on
		 * hw.dri.$n.name.
		 */
		busid_format = "%*s %*s pci:%d:%d:%d.%d";
		sysctl_value_len = sizeof(sysctl_value);
		memset(sysctl_value, 0, sysctl_value_len);
		sprintf(sysctl_name, "hw.dri.%d.name", dev);
		ret = sysctlbyname(sysctl_name, sysctl_value, &sysctl_value_len,
		    NULL, 0);
	}

	if (ret != 0)
		return (-1);

	ret = sscanf(sysctl_value, busid_format,
	    domain, bus, slot, function);
	if (ret != 4) {
		errno = ENOENT;
		return (-1);
	}
	
	return (0);
}

int
devq_device_get_pciid_full_from_fd(int fd,
    int *vendor_id, int *device_id, int *subvendor_id,
    int *subdevice_id, int *revision_id)
{
	int i, ret, dev, domain, bus, slot, function;
	char sysctl_name[32], sysctl_value[128];
	const char *busid_format;
	size_t sysctl_value_len;

	/*
	 * FIXME: This function is specific to DRM devices.
	 */

	/*
	 * We don't need the driver name, but this function already
	 * walks the hw.dri.* tree and returns the number in
	 * hw.dri.$number.
	 */
	dev = devq_device_drm_get_drvname_from_fd(fd, NULL, NULL);
	if (dev < 0)
		return (-1);

	/*
	 * Read the hw.dri.$n.busid sysctl to get the location of the
	 * device on the PCI bus. We can then use this location to find
	 * the corresponding dev.vgapci.$m tree.
	 */
	sprintf(sysctl_name, "hw.dri.%d.busid", dev);

	busid_format = "pci:%d:%d:%d.%d";
	sysctl_value_len = sizeof(sysctl_value);
	memset(sysctl_value, 0, sysctl_value_len);
	ret = sysctlbyname(sysctl_name, sysctl_value, &sysctl_value_len,
	    NULL, 0);

	if (ret != 0) {
		/*
		 * If hw.dri.$n.busid isn't available, fallback on
		 * hw.dri.$n.name.
		 */
		busid_format = "%*s %*s pci:%d:%d:%d.%d";
		sysctl_value_len = sizeof(sysctl_value);
		memset(sysctl_value, 0, sysctl_value_len);
		sprintf(sysctl_name, "hw.dri.%d.name", dev);
		ret = sysctlbyname(sysctl_name, sysctl_value, &sysctl_value_len,
		    NULL, 0);
	}

	if (ret != 0)
		return (-1);

	ret = sscanf(sysctl_value, busid_format,
	    &domain, &bus, &slot, &function);
	if (ret != 4) {
		errno = ENOENT;
		return (-1);
	}

	/*
	 * Now, look at all dev.vgapci.$m trees until we find the
	 * correct device. We specifically look at:
	 *     o  dev.vgapci.$m.%location
	 *     o  dev.vgapci.$m.%parent
	 */
	for (i = 0; i < DEVQ_MAX_DEVS; ++i) {
		int tmp_domain, tmp_bus, tmp_slot, tmp_function;

		ret = devq_compare_vgapci_busaddr(i, &tmp_domain, &tmp_bus,
		    &tmp_slot, &tmp_function);

		if (ret == 0 &&
		    tmp_domain == domain &&
		    tmp_bus == bus &&
		    tmp_slot == slot &&
		    tmp_function == function)
			break;
	}

	if (i == DEVQ_MAX_DEVS) {
		errno = ENOENT;
		return (-1);
	}

	/*
	 * Ok, we have the right tree. Let's read dev.vgapci.$m.%pnpinfo
	 * to gather the PCI ID.
	 */
	sprintf(sysctl_name, "dev.vgapci.%d.%%pnpinfo", i);

	sysctl_value_len = sizeof(sysctl_value);
	memset(sysctl_value, 0, sysctl_value_len);
	ret = sysctlbyname(sysctl_name, sysctl_value,
	    &sysctl_value_len, NULL, 0);
	if (ret != 0)
		return (-1);

	ret = sscanf(sysctl_value, "vendor=0x%04x device=0x%04x subvendor=0x%04x subdevice=0x%04x",
	    vendor_id, device_id, subvendor_id, subdevice_id);
	if (ret != 4) {
		errno = EINVAL;
		return (-1);
	}

	/* XXX: add code to find out revision id */
	revision_id = 0;

	return (0);
}

int
devq_device_get_pciid_from_fd(int fd,
    int *vendor_id, int *device_id)
{
	int subvendor_id, subdevice_id, revision_id;

	return devq_device_get_pciid_full_from_fd(fd,
		vendor_id, device_id, &subvendor_id,
		&subdevice_id, &revision_id);
}

