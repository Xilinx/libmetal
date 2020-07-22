/*
 * Copyright (c) 2015, Xilinx Inc. and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * @file	linux/device.c
 * @brief	Linux libmetal device operations.
 */

#include <metal/device.h>
#include <metal/sys.h>
#include <metal/utilities.h>
#include <metal/irq.h>
#include <metal/shmem.h>
#include <sys/ioctl.h>
#include <byteswap.h>
#include "uio.h"

static struct linux_bus *to_linux_bus(struct metal_bus *bus)
{
	return metal_container_of(bus, struct linux_bus, bus);
}

static struct linux_device *to_linux_device(struct metal_device *device)
{
	return metal_container_of(device, struct linux_device, device);
}

static int metal_read_attr(char *attr_path, unsigned long *value)
{
	int length, fd;
	char *val;
	char buf[1024];

	if ((fd = open(attr_path, O_RDONLY)) < 0)
		return -EINVAL;

	length = read(fd, buf, 1024);
	if (length <= 0) {
		close(fd);
		return -EINVAL;
	}

	val = (char  *)malloc(length + 1);
	if (!val)
		return -ENOMEM;

	memcpy(val, buf, length);
	val[length] = '\0';
	*value = strtoul(val, NULL, 0);

	close(fd);
	free(val);

	return 0;
}

static int metal_uio_read_map_attr(struct linux_device *ldev, unsigned index,
				   const char *name, unsigned long *value)
{
	const char *cls = ldev->cls_path;
	char path[PATH_MAX];
	int result;

	result = snprintf(path, sizeof(path),
			  "%s/maps/map%u/%s", cls, index, name);
	if (result >= (int)sizeof(path)) {
		metal_log(METAL_LOG_ERROR,
			  "%s: %d: path exceeding %d bytes\n",
			   __func__, __LINE__, sizeof(path));
		return -EOVERFLOW;
	}

	result = metal_read_attr(path, value);
	if (result < 0) {
		metal_log(METAL_LOG_WARNING,
			  "Error reading from attribute %s\n", path);
		return result;
	}

	return 0;
}

static int metal_uio_dev_bind(struct linux_device *ldev,
			      struct linux_driver *ldrv)
{
	const char *drv_name;
	char command[PATH_MAX];
	char path[PATH_MAX];
	int result;

	drv_name = udev_device_get_driver(ldev->udev_device);
	if (!drv_name || strcmp(drv_name, ldrv->drv_name)) {
		metal_log(METAL_LOG_ERROR,
			  "Bound to incompatible driver: %s expected: %s\n",
			  drv_name, ldrv->drv_name);

		if (drv_name != NULL ) {
			result = snprintf(path, sizeof(path),
					  "/sys/bus/platform/devices/%s/driver",
					  ldev->dev_name);
			if (result >= (int)sizeof(path)) {
				metal_log(METAL_LOG_ERROR,
					  "%s: %d: path exceeding %d bytes\n",
					  __func__, __LINE__, sizeof(path));
				return -EOVERFLOW;
			}

			if (!metal_check_file_available(path)) {
				result = snprintf(command, sizeof(command),
						"%s/unbind", path);
				if (result >= (int)sizeof(command))
					return -EOVERFLOW;

				result = metal_linux_exec_cmd(ldev->dev_name,
							      command);
				if (result)
					return result;
			}
		}

		result = snprintf(command, sizeof(command),
				  "/sys/bus/platform/devices/%s/driver_override",
				  ldev->dev_name);
		if (result >= (int)sizeof(command)) {
			metal_log(METAL_LOG_ERROR,
				  "%s: %d: command exceeding %d bytes\n",
				  __func__, __LINE__, sizeof(command));
			return -EOVERFLOW;
		}

		result = metal_linux_exec_cmd(ldrv->drv_name, command);
		if (result)
			return result;

		result = snprintf(command, sizeof(command),
				  "/sys/bus/platform/drivers_probe");
		if (result >= (int)sizeof(command)) {
			metal_log(METAL_LOG_ERROR,
				  "%s: %d: command exceeding %d bytes\n",
				  __func__, __LINE__, sizeof(command));
			return -EOVERFLOW;
		}

		result = metal_linux_exec_cmd(ldev->dev_name, command);
		if (result)
			return result;
	}

	metal_log(METAL_LOG_DEBUG, "bound device %s to driver %s\n",
		  ldev->dev_name, ldrv->drv_name);

	return 0;
}

static int metal_uio_dev_open(struct linux_bus *lbus, struct linux_device *ldev)
{
	char path[PATH_MAX];
	struct linux_driver *ldrv = ldev->ldrv;
	unsigned long *phys, offset=0, size=0;
	struct metal_io_region *io;
	const char *syspath_ptr;
	struct udev_enumerate *enumerate;
	struct udev_list_entry *list_entry;
	struct udev_device *device;
	const char *sys_name;
	const char *dev_name;
	const char *str;
	int result, i;
	void *virt;
	int irq_info;

	ldev->fd = -1;
	ldev->device.irq_info = (void *)-1;

	ldev->udev = udev_new();
	if (!ldev->udev) {
		metal_log(METAL_LOG_ERROR, "%s: failed to allocated udev\n",
			  __func__);
		return -ENODEV;
	}

	ldev->udev_device = udev_device_new_from_subsystem_sysname(ldev->udev,
				lbus->bus_name, ldev->dev_name);
	if (!ldev->udev_device) {
		udev_unref(ldev->udev);
		metal_log(METAL_LOG_ERROR, "%s: udev_device %s:%s not found\n",
			  __func__, lbus->bus_name, ldev->dev_name);
		return -ENODEV;
	}

	syspath_ptr = udev_device_get_syspath(ldev->udev_device);
	result = snprintf(ldev->sys_path, sizeof(ldev->sys_path),
			  "%s", syspath_ptr);
	if (result >= (int)sizeof(ldev->sys_path)) {
		udev_device_unref(ldev->udev_device);
		udev_unref(ldev->udev);
		ldev->udev_device = NULL;
		ldev->udev = NULL;
		metal_log(METAL_LOG_ERROR,
			  "%s: %d: path exceeding %d bytes\n",
			  __func__, __LINE__, sizeof(ldev->sys_path));
		return -EOVERFLOW;
	}

	result = metal_uio_dev_bind(ldev, ldrv);
	if (result) {
		if (ldev->udev_device) {
			udev_device_unref(ldev->udev_device);
			udev_unref(ldev->udev);
			ldev->udev_device = NULL;
			ldev->udev = NULL;
		}

		return result;
	}

	result = snprintf(path, sizeof(path), "%s/uio", ldev->sys_path);
	if (result >= (int)sizeof(path)) {
		metal_log(METAL_LOG_ERROR,
			  "%s: %d: path exceeding %d bytes\n",
			  __func__, __LINE__, sizeof(path));
		return -EOVERFLOW;
	}

	enumerate = udev_enumerate_new(ldev->udev);
	udev_enumerate_add_match_subsystem(enumerate, "uio");
	udev_enumerate_scan_devices(enumerate);

	udev_list_entry_foreach(list_entry, udev_enumerate_get_list_entry(enumerate)) {

		device = udev_device_new_from_syspath(udev_enumerate_get_udev(enumerate),
				udev_list_entry_get_name(list_entry));
		str = udev_device_get_syspath(device);
		if ( strstr(str, path) ) {
			sys_name = udev_device_get_syspath(device);
			dev_name = udev_device_get_devnode(device);

			result = snprintf(ldev->cls_path, sizeof(ldev->cls_path),
					"%s", sys_name);
			if (result >= (int)sizeof(ldev->cls_path)) {
				metal_log(METAL_LOG_ERROR,
					  "%s: %d: path exceeding %d bytes\n",
					  __func__, __LINE__, sizeof(ldev->cls_path));
				return -EOVERFLOW;
			}

			result = snprintf(ldev->dev_path, sizeof(ldev->dev_path),
					"%s", dev_name);
			if (result >= (int)sizeof(ldev->dev_path)) {
			metal_log(METAL_LOG_ERROR,
				  "%s: %d: path exceeding %d bytes\n",
				  __func__, __LINE__, sizeof(ldev->dev_path));
				return -EOVERFLOW;
			}
		}

		udev_device_unref(device);
	}

	i = 0;
	do {
		if (!access(ldev->dev_path, F_OK))
			break;
		usleep(10);
		i++;
	} while (i < 1000);
	if (i >= 1000) {
		metal_log(METAL_LOG_ERROR, "failed to open file %s, timeout.\n",
			  ldev->dev_path);
		return -ENODEV;
	}
	result = metal_open(ldev->dev_path, 0);
	if (result < 0) {
		metal_log(METAL_LOG_ERROR, "failed to open device %s\n",
			  ldev->dev_path, strerror(-result));
		return result;
	}
	ldev->fd = result;

	metal_log(METAL_LOG_DEBUG, "opened %s:%s as %s\n",
		  lbus->bus_name, ldev->dev_name, ldev->dev_path);

	for (i = 0, result = 0; !result && i < METAL_MAX_DEVICE_REGIONS; i++) {
		phys = &ldev->region_phys[ldev->device.num_regions];
		result = (result ? result :
			 metal_uio_read_map_attr(ldev, i, "offset", &offset));
		result = (result ? result :
			 metal_uio_read_map_attr(ldev, i, "addr", phys));
		result = (result ? result :
			 metal_uio_read_map_attr(ldev, i, "size", &size));
		result = (result ? result :
			 metal_map(ldev->fd, offset, size, 0, 0, &virt));
		if (!result) {
			io = &ldev->device.regions[ldev->device.num_regions];
			metal_io_init(io, virt, phys, size, -1, 0, NULL);
			ldev->device.num_regions++;
		}
	}

	irq_info = 1;
	if (write(ldev->fd, &irq_info, sizeof(irq_info)) <= 0) {
		metal_log(METAL_LOG_INFO,
			  "%s: No IRQ for device %s.\n",
			  __func__, ldev->dev_name);
		ldev->device.irq_num =  0;
		ldev->device.irq_info = (void *)-1;
	} else {
		ldev->device.irq_num =  1;
		ldev->device.irq_info = (void *)(intptr_t)ldev->fd;
		metal_linux_irq_register_dev(&ldev->device, ldev->fd);
	}

	return 0;
}

static void metal_uio_dev_close(struct linux_bus *lbus,
				struct linux_device *ldev)
{
	char command[PATH_MAX];
	char path[PATH_MAX];
	int result;

	(void)lbus;

	if (ldev->udev_device) {
		udev_device_unref(ldev->udev_device);
		udev_unref(ldev->udev);
		ldev->udev_device = NULL;
		ldev->udev = NULL;
	}

	if (ldev->fd >= 0) {
		close(ldev->fd);
	}

	result = snprintf(path, sizeof(path),
			  "/sys/bus/platform/devices/%s/driver", ldev->dev_name);
	if (result >= (int)sizeof(path)) {
		metal_log(METAL_LOG_ERROR,
			  "%s: %d: Path exceeding %d bytes\n",
			  __func__, __LINE__, sizeof(path));
		return;
	}

	if (!metal_check_file_available(path)) {
		result = snprintf(command, PATH_MAX,
				  "%s/unbind", path);
		if (result >= (int)PATH_MAX) {
			metal_log(METAL_LOG_ERROR,
				  "%s: %d: command exceeding %d bytes\n",
				  __func__, __LINE__, PATH_MAX);
			return;
		}

		metal_linux_exec_cmd(ldev->dev_name, command);
	}

	result = snprintf(command, PATH_MAX,
			  "/sys/bus/platform/devices/%s/driver_override",
			  ldev->dev_name);
	if (result >= (int)PATH_MAX) {
		metal_log(METAL_LOG_ERROR,
			  "%s: %d: command exceeding %d bytes\n",
			  __func__, __LINE__, PATH_MAX);
		return;
	}

	metal_linux_exec_cmd(" ", command);
}

static void metal_uio_dev_irq_ack(struct linux_bus *lbus,
				 struct linux_device *ldev,
				 int irq)
{
	(void)lbus;
	(void)irq;
	int irq_info = 1;
	unsigned int val;
	int ret;

	ret = read(ldev->fd, (void *)&val, sizeof(val));
	if (ret < 0) {
		metal_log(METAL_LOG_ERROR, "%s, read uio irq fd %d failed: %d.\n",
						__func__, ldev->fd, ret);
		return;
	}
	ret = write(ldev->fd, &irq_info, sizeof(irq_info));
	if (ret < 0) {
		metal_log(METAL_LOG_ERROR, "%s, write uio irq fd %d failed: %d.\n",
						__func__, ldev->fd, errno);
	}
}

static int metal_uio_dev_dma_map(struct linux_bus *lbus,
				 struct linux_device *ldev,
				 uint32_t dir,
				 struct metal_sg *sg_in,
				 int nents_in,
				 struct metal_sg *sg_out)
{
	int i, j;
	void *vaddr_sg_lo, *vaddr_sg_hi, *vaddr_lo, *vaddr_hi;
	struct metal_io_region *io;

	(void)lbus;
	(void)dir;

	/* Check if the the input virt address is MMIO address */
	for (i = 0; i < nents_in; i++) {
		vaddr_sg_lo = sg_in[i].virt;
		vaddr_sg_hi = vaddr_sg_lo + sg_in[i].len;
		for (j = 0, io = ldev->device.regions;
		     j < (int)ldev->device.num_regions; j++, io++) {
			vaddr_lo = io->virt;
			vaddr_hi = vaddr_lo + io->size;
			if (vaddr_sg_lo >= vaddr_lo &&
			    vaddr_sg_hi <= vaddr_hi) {
				break;
			}
		}
		if (j == (int)ldev->device.num_regions) {
			metal_log(METAL_LOG_WARNING,
			  "%s,%s: input address isn't MMIO addr: 0x%x,%d.\n",
			__func__, ldev->dev_name, vaddr_sg_lo, sg_in[i].len);
			return -EINVAL;
		}
	}
	if (sg_out != sg_in)
		memcpy(sg_out, sg_in, nents_in*(sizeof(struct metal_sg)));
	return nents_in;
}

static void metal_uio_dev_dma_unmap(struct linux_bus *lbus,
				 struct linux_device *ldev,
				 uint32_t dir,
				 struct metal_sg *sg,
				 int nents)
{
	(void) lbus;
	(void) ldev;
	(void) dir;
	(void) sg;
	(void) nents;
	return;
}

static int metal_uio_dev_shm_attach(struct linux_bus *lbus,
				    struct linux_device *ldev,
				    struct metal_generic_shmem *shm,
				    unsigned int direction,
				    struct metal_shm_ref *ref)
{
	int ret;
	struct uio_dmabuf_args args;
	struct linux_device_shm_io_region *linux_io = NULL;;
	void *va;

	(void)lbus;

	va = mmap(NULL, shm->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		  shm->id, 0);
	if (va == MAP_FAILED) {
		metal_log(METAL_LOG_ERROR,
			  "%s: failed to mmap shmem %s, of 0x%lx\n",
			  __func__, shm->name, shm->size);
		return -EINVAL;
	}
	linux_io = malloc(sizeof(*linux_io));
	if (linux_io == NULL) {
		metal_log(METAL_LOG_ERROR,
			  "%s: failed to allocate memory.\n", __func__);
		ret = -ENOMEM;
		goto error;
	}
	args.dbuf_fd = shm->id;
	if (direction == METAL_SHM_DIR_DEV_R)
		args.dir = UIO_DMABUF_DIR_TO_DEV;
	else if (direction == METAL_SHM_DIR_DEV_W)
		args.dir = UIO_DMABUF_DIR_FROM_DEV;
	else
		args.dir = UIO_DMABUF_DIR_BIDIR;
	ret = ioctl(ldev->fd, UIO_IOC_MAP_DMABUF, &args);
	if (ret < 0) {
		metal_log(METAL_LOG_ERROR,
			  "%s: Failed to attach shared memory to UIO %s.\n",
			  __func__, ldev->dev_name);
		goto error;
	}
	linux_io->phys = args.dma_addr;
	metal_io_init(&linux_io->io, va, &linux_io->phys, shm->size,
		      -1, 0, NULL);
	ref->sg.ios = &linux_io->io;
	ref->sg.nents = 1;
	return 0;
error:
	if (linux_io != NULL) {
		free(linux_io);
	}
	munmap(va, shm->size); 
	return ret;
}

static void metal_uio_dev_shm_detach(struct linux_bus *lbus,
				     struct linux_device *ldev,
				     struct metal_generic_shmem *shm,
				     struct metal_shm_ref *ref)
{
	int ret;
	struct uio_dmabuf_args args;
	struct metal_io_region *io;
	void *va;

	(void)lbus;
	args.dbuf_fd = shm->id;
	ret = ioctl(ldev->fd, UIO_IOC_UNMAP_DMABUF, &args);
	if (ret < 0) {
		metal_log(METAL_LOG_ERROR,
			  "%s: Failed to detach shared memory to UIO %s.\n",
			  __func__, ldev->dev_name);
	}
	ref->dev = NULL;
	ref->sg.nents = 0;
	io = ref->sg.ios;
	va = metal_io_virt(io, 0);
	if (va != NULL) {
		munmap(va, metal_io_region_size(io));
	}			
	free(ref->sg.ios);
}

static struct linux_bus linux_bus[] = {
	{
		.bus_name	= "platform",
		.drivers = {
			{
				.drv_name  = "uio_pdrv_genirq",
				.mod_name  = "uio_pdrv_genirq",
				.cls_name  = "uio",
				.dev_open  = metal_uio_dev_open,
				.dev_close = metal_uio_dev_close,
				.dev_irq_ack  = metal_uio_dev_irq_ack,
				.dev_dma_map = metal_uio_dev_dma_map,
				.dev_dma_unmap = metal_uio_dev_dma_unmap,
				.dev_shm_attach = metal_uio_dev_shm_attach,
				.dev_shm_detach = metal_uio_dev_shm_detach,
			},
			{
				.drv_name  = "uio_dmem_genirq",
				.mod_name  = "uio_dmem_genirq",
				.cls_name  = "uio",
				.dev_open  = metal_uio_dev_open,
				.dev_close = metal_uio_dev_close,
				.dev_irq_ack  = metal_uio_dev_irq_ack,
				.dev_dma_map = metal_uio_dev_dma_map,
				.dev_dma_unmap = metal_uio_dev_dma_unmap,
				.dev_shm_attach = metal_uio_dev_shm_attach,
				.dev_shm_detach = metal_uio_dev_shm_detach,
			},
			{ 0 /* sentinel */ }
		}
	},
	{
		.bus_name	= "pci",
		.drivers = {
			{
				.drv_name  = "vfio-pci",
				.mod_name  = "vfio-pci",
			},
			{
				.drv_name  = "uio_pci_generic",
				.mod_name  = "uio_pci_generic",
				.cls_name  = "uio",
				.dev_open  = metal_uio_dev_open,
				.dev_close = metal_uio_dev_close,
				.dev_irq_ack  = metal_uio_dev_irq_ack,
				.dev_dma_map = metal_uio_dev_dma_map,
				.dev_dma_unmap = metal_uio_dev_dma_unmap,
				.dev_shm_attach = metal_uio_dev_shm_attach,
				.dev_shm_detach = metal_uio_dev_shm_detach,
			},
			{ 0 /* sentinel */ }
		}
	},
	{
		/* sentinel */
		.bus_name = NULL,
	},
};

#define for_each_linux_bus(lbus)					\
	for ((lbus) = linux_bus; (lbus)->bus_name; (lbus)++)
#define for_each_linux_driver(lbus, ldrv)			\
	for ((ldrv) = lbus->drivers; (ldrv)->drv_name; (ldrv)++)


static int metal_linux_dev_open(struct metal_bus *bus,
				const char *dev_name,
				struct metal_device **device)
{
	struct linux_bus *lbus = to_linux_bus(bus);
	struct linux_device *ldev = NULL;
	struct linux_driver *ldrv;
	int error;

	ldev = malloc(sizeof(*ldev));
	if (!ldev)
		return -ENOMEM;

	for_each_linux_driver(lbus, ldrv) {

		/* Check if we have a viable driver. */
		if ((ldrv->is_drv_ready == false) || !ldrv->dev_open)
			continue;

		/* Allocate a linux device if we haven't already. */
		if (!ldev)
			ldev = malloc(sizeof(*ldev));
		if (!ldev)
			return -ENOMEM;

		/* Reset device data. */
		memset(ldev, 0, sizeof(*ldev));
		strncpy(ldev->dev_name, dev_name, sizeof(ldev->dev_name) - 1);
		ldev->fd = -1;
		ldev->ldrv = ldrv;
		ldev->device.bus = bus;

		/* Try and open the device. */
		error = ldrv->dev_open(lbus, ldev);
		if (error) {
			ldrv->dev_close(lbus, ldev);
			continue;
		}

		*device = &ldev->device;
		(*device)->name = ldev->dev_name;

		metal_list_add_tail(&bus->devices, &(*device)->node);
		return 0;
	}

	if (ldev)
		free(ldev);

	return -ENODEV;
}

static void metal_linux_dev_close(struct metal_bus *bus,
				  struct metal_device *device)
{
	struct linux_device *ldev = to_linux_device(device);
	struct linux_bus *lbus = to_linux_bus(bus);

	ldev->ldrv->dev_close(lbus, ldev);
	metal_list_del(&device->node);
	free(ldev);
}

static void metal_linux_bus_close(struct metal_bus *bus)
{
	struct linux_bus *lbus = to_linux_bus(bus);
	struct linux_driver *ldrv;

	for_each_linux_driver(lbus, ldrv) {
		ldrv->is_drv_ready = false;
	}
}

static void metal_linux_dev_irq_ack(struct metal_bus *bus,
			     struct metal_device *device,
			     int irq)
{
	struct linux_device *ldev = to_linux_device(device);
	struct linux_bus *lbus = to_linux_bus(bus);

	return ldev->ldrv->dev_irq_ack(lbus, ldev, irq);
}

static int metal_linux_dev_dma_map(struct metal_bus *bus,
			     struct metal_device *device,
			     uint32_t dir,
			     struct metal_sg *sg_in,
			     int nents_in,
			     struct metal_sg *sg_out)
{
	struct linux_device *ldev = to_linux_device(device);
	struct linux_bus *lbus = to_linux_bus(bus);

	return ldev->ldrv->dev_dma_map(lbus, ldev, dir, sg_in,
				       nents_in, sg_out);
}

static void metal_linux_dev_dma_unmap(struct metal_bus *bus,
			        struct metal_device *device,
			        uint32_t dir,
			        struct metal_sg *sg,
			        int nents)
{
	struct linux_device *ldev = to_linux_device(device);
	struct linux_bus *lbus = to_linux_bus(bus);

	ldev->ldrv->dev_dma_unmap(lbus, ldev, dir, sg,
				       nents);
}

static int metal_linux_dev_shm_attach(struct metal_bus *bus,
				      struct metal_generic_shmem *shm,
				      struct metal_device *device,
				      unsigned int direction,
				      struct metal_shm_ref *ref)
{
	struct linux_device *ldev = to_linux_device(device);
	struct linux_bus *lbus = to_linux_bus(bus);

	if (ldev->ldrv->dev_shm_attach) {
		return ldev->ldrv->dev_shm_attach(lbus, ldev, shm,
						  direction, ref);
	} else {
		metal_log(METAL_LOG_ERROR,
			  "No device driver definition for shm attach.\n");
		return -EINVAL;
	}
}

static void metal_linux_dev_shm_detach(struct metal_bus *bus,
				       struct metal_generic_shmem *shm,
				       struct metal_device *device,
				       struct metal_shm_ref *ref)
{
	struct linux_device *ldev = to_linux_device(device);
	struct linux_bus *lbus = to_linux_bus(bus);

	if (ldev->ldrv->dev_shm_detach) {
		ldev->ldrv->dev_shm_detach(lbus, ldev, shm, ref);
	} else {
		metal_log(METAL_LOG_ERROR,
			  "No device driver definition for shm detach.\n");
	}
}

static const struct metal_bus_ops metal_linux_bus_ops = {
	.bus_close	= metal_linux_bus_close,
	.dev_open	= metal_linux_dev_open,
	.dev_close	= metal_linux_dev_close,
	.dev_irq_ack	= metal_linux_dev_irq_ack,
	.dev_dma_map	= metal_linux_dev_dma_map,
	.dev_dma_unmap	= metal_linux_dev_dma_unmap,
	.dev_shm_attach = metal_linux_dev_shm_attach,
	.dev_shm_detach = metal_linux_dev_shm_detach,
};

static int metal_linux_register_bus(struct linux_bus *lbus)
{
	lbus->bus.name = lbus->bus_name;
	lbus->bus.ops  = metal_linux_bus_ops;
	return metal_bus_register(&lbus->bus);
}

static int metal_linux_probe_driver(struct linux_bus *lbus,
				    struct linux_driver *ldrv)
{
	char command[PATH_MAX];
	int ret, fd;

	/* Check if the required driver is probed */
	ret = snprintf(command, sizeof(command),
		       "/sys/bus/%s/drivers/%s", lbus->bus_name, ldrv->mod_name);
	if (ret >= (int)sizeof(command)) {
		metal_log(METAL_LOG_ERROR,
			  "%s: %d: command exceeding %d bytes\n",
			  __func__, __LINE__, sizeof(command));
		return -EOVERFLOW;
	}

	fd = open(command, O_RDONLY);
	if ( fd < 0 ) {
		ldrv->is_drv_ready = false;
	} else {
		close(fd);
		ldrv->is_drv_ready = true;
	}

	/* Try probing the module and then open the driver. */
	if (ldrv->is_drv_ready == false) {
		ret = snprintf(command, sizeof(command),
			       "modprobe %s > /dev/null 2>&1", ldrv->mod_name);
		if (ret >= (int)sizeof(command)) {
			metal_log(METAL_LOG_ERROR,
				  "%s: %d: command exceeding %d bytes\n",
				  __func__, __LINE__, sizeof(command));
			return -EOVERFLOW;
		}

		ret = system(command);
		if (ret < 0) {
			metal_log(METAL_LOG_WARNING,
				  "%s: executing system command '%s' failed.\n",
				  __func__, command);
		}
		ldrv->is_drv_ready = true;
	}

	/* Try sudo probing the module and then open the driver. */
	if (ldrv->is_drv_ready == false) {
		ret = snprintf(command, sizeof(command),
			       "sudo modprobe %s > /dev/null 2>&1", ldrv->mod_name);
		if (ret >= (int)sizeof(command))
			return -EOVERFLOW;
		ret = system(command);
		if (ret < 0) {
			metal_log(METAL_LOG_WARNING,
				  "%s: executing system command '%s' failed.\n",
				  __func__, command);
		}
		ldrv->is_drv_ready = true;
	}

	/* If all else fails... */
	return (ldrv->is_drv_ready == true)? 0 : -ENODEV;
}

static void metal_linux_bus_close(struct metal_bus *bus);

static int metal_linux_probe_bus(struct linux_bus *lbus)
{
	struct linux_driver *ldrv;
	int ret, fd, error = -ENODEV;
	char path[PATH_MAX];

	ret = snprintf(path, sizeof(path), "/sys/bus/%s", lbus->bus_name);
	if (ret >= (int)sizeof(path)) {
		metal_log(METAL_LOG_ERROR,
			  "%s: %d: path greater than %d bytes.\n",
			  __func__, __LINE__, sizeof(path));
		return -EOVERFLOW;
	}

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -ENODEV;
	else
		close(fd);

	for_each_linux_driver(lbus, ldrv) {
		ret = metal_linux_probe_driver(lbus, ldrv);
		/* Clear the error if any driver is available */
		if (!ret)
			error = ret;
	}

	if (error) {
		metal_linux_bus_close(&lbus->bus);
		return error;
	}

	error = metal_linux_register_bus(lbus);
	if (error)
		metal_linux_bus_close(&lbus->bus);

	return error;
}

int metal_linux_bus_init(void)
{
	struct linux_bus *lbus;
	int valid = 0;

	for_each_linux_bus(lbus)
		valid += metal_linux_probe_bus(lbus) ? 0 : 1;

	return valid ? 0 : -ENODEV;
}

void metal_linux_bus_finish(void)
{
	struct linux_bus *lbus;
	struct metal_bus *bus;

	for_each_linux_bus(lbus) {
		if (metal_bus_find(lbus->bus_name, &bus) == 0)
			metal_bus_unregister(bus);
	}
}

int metal_generic_dev_sys_open(struct metal_device *dev)
{
	(void)dev;
	return 0;
}

int metal_linux_exec_cmd(const char *command, char *path)
{
	int fd, ret;

	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -EINVAL;

	ret = write(fd, command, strlen(command));
	if (ret < 0) {
		metal_log(METAL_LOG_WARNING,
			"%s: %d: executing system command '%s' failed.\n",
			__func__, __LINE__, command);
		return -EINVAL;
	}

	close(fd);

	return 0;
}

int metal_linux_get_device_property(struct metal_device *device,
				    const char *property_name,
				    void *output, int len)
{
	int fd = 0 , status = 0, result = 0;
	const int flags = O_RDONLY;
	const int mode = S_IRUSR | S_IRGRP | S_IROTH;
	struct linux_device *ldev = to_linux_device(device);
	char path[PATH_MAX];

	result = snprintf(path, sizeof(path), "%s/of_node/%s",
			  ldev->sys_path, property_name);
	if (result >= (int)sizeof(path)) {
		metal_log(METAL_LOG_ERROR,
			  "%s: %d: path greater than %d bytes.\n",
			  __func__, __LINE__, sizeof(path));
		return -EOVERFLOW;
	}

	fd = open(path, flags, mode);
	if (fd < 0)
		return -errno;
	status = read(fd, output, len);

	return status < 0 ? -errno : 0;
}

/* Set the device's private data */
void metal_device_set_pdata(struct linux_device *device, void *pdata)
{
	device->priv_data = pdata;
}

/* Set the device's DMA addressing capability limit */
void metal_device_set_dmacap(struct metal_device *device, int val) {
	struct linux_device *ptr;
	ptr = metal_container_of(device, struct linux_device, device);
	ptr->dma_cap = val;
}

/* Get the device's DMA addressing capability limit */
int metal_device_get_dmacap(struct metal_device *device) {
	struct linux_device *ptr;
	ptr = metal_container_of(device, struct linux_device, device);
	return ptr->dma_cap;
}

int metal_check_file_available(char *path)
{
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		/* File unavailable */
		return -EINVAL;
	} else {
		/* File available */
		close(fd);
	}

	return 0;
}
