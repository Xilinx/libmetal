#ifndef  __METAL_LINUX_VFIO__H__
#define  __METAL_LINUX_VFIO__H__

#include "device.h"

int metal_vfio_dev_open(struct linux_bus *lbus, struct linux_device *ldev);
void metal_vfio_dev_close(struct linux_bus *lbus, struct linux_device *ldev);
void metal_vfio_dev_irq_ack(struct linux_bus *lbus, struct linux_device *ldev,
			   int irq);
int metal_vfio_dev_dma_map(struct linux_bus *lbus, struct linux_device *ldev,
			   uint32_t dir, struct metal_sg *sg_in, int nents_in,
			   struct metal_sg *sg_out);
void metal_vfio_dev_dma_unmap(struct linux_bus *lbus, struct linux_device *ldev,
			       uint32_t dir, struct metal_sg *sg, int nents);
#endif /* __METAL_LINUX_VFIO__H__ */
