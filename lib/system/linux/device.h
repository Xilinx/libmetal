#ifndef __METAL_LINUX_DEVICE__H__
#define __METAL_LINUX_DEVICE__H__

#include <metal/device.h>
#include <metal/sys.h>
#include <metal/utilities.h>
#include <metal/irq.h>
#include <metal/shmem.h>
#include <sys/ioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_DRIVERS	64

struct linux_bus;
struct linux_device;

struct linux_driver {
	const char		*drv_name;
	const char		*mod_name;
	const char		*cls_name;
	struct sysfs_driver	*sdrv;
	int			(*dev_open)(struct linux_bus *lbus,
					    struct linux_device *ldev);
	void			(*dev_close)(struct linux_bus *lbus,
					     struct linux_device *ldev);
	void			(*dev_irq_ack)(struct linux_bus *lbus,
					     struct linux_device *ldev,
					     int irq);
	int			(*dev_dma_map)(struct linux_bus *lbus,
						struct linux_device *ldev,
						uint32_t dir,
						struct metal_sg *sg_in,
						int nents_in,
						struct metal_sg *sg_out);
	void			(*dev_dma_unmap)(struct linux_bus *lbus,
						struct linux_device *ldev,
						uint32_t dir,
						struct metal_sg *sg,
						int nents);
	int			(*dev_shm_attach)(struct linux_bus *lbus,
						  struct linux_device *ldev,
						  struct metal_generic_shmem *shm,
						  unsigned int direction,
						  struct metal_shm_ref *ref);
	void			(*dev_shm_detach)(struct linux_bus *lbus,
						  struct linux_device *ldev,
						  struct metal_generic_shmem *shm,
						  struct metal_shm_ref *ref);
};

struct linux_bus {
	struct metal_bus	bus;
	const char		*bus_name;
	struct linux_driver	drivers[MAX_DRIVERS];
	struct sysfs_bus	*sbus;
};

struct linux_device_shm_io_region {
	struct metal_io_region io;
	metal_phys_addr_t phys;
};

struct linux_device {
	struct metal_device		device;
	char				dev_name[PATH_MAX];
	char				dev_path[PATH_MAX];
	char				cls_path[PATH_MAX];
	metal_phys_addr_t		region_phys[METAL_MAX_DEVICE_REGIONS];
	struct linux_driver		*ldrv;
	struct sysfs_device		*sdev;
	struct sysfs_attribute		*override;
	int				fd;
	/* Store the dma addressing capability of device */
	int				dma_cap;
};

/* Set the device's DMA addressing capability limit */
extern void metal_device_set_dmacap(struct metal_device *device, int val);
/* Get the device's DMA addressing capability limit */
extern int metal_device_get_dmacap(struct metal_device *device);

#ifdef __cplusplus
}
#endif

#endif /* __METAL_LINUX_DEVICE__H__ */
