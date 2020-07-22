/*
 * Adding VFIO (Vitual Function IO) support enables userspace applications to
 * allocate memory and pass the virtual address to VFIO framework, which maps
 * the virtual address to IO Virtual Address(IOVA). The userspace applications
 * can now use IOVA address for dma operations.
 * The user space application allocates memory and makes use of metal_dma_map()
 * and metal_dma_unmap() APIs to convert the allocated userspace memory to the
 * dma-able memory using VFIO.
 * Before calling metal_dma_map(), memory allocated needs to be PAGE_SIZE
 * aligned and which can be allocated using aligned_alloc or mmap calls. The
 * memory allocated (that can be used for DMA operations) needs to be
 * populated into the struct metal_sg *sg_in (passed as an argument). Here
 * sg_in.virt represents the virtual address of the memory allocated and
 * sg_in.size represents the total size of the memory. This metal_dma_map()
 * API internally calls the driver->dev_dma_map().The dev_dma_map() internally
 * calls VFIO dev_map(). This VFIO dev_map() checks for device's dma capability
 * using metal_device_get_dmacap(struct metal_device *device) API. If the
 * device is 64bit capable, then VFIO dev_map() uses the same userspace Virtual
 * Address as the IOVA address(IO Virtual Address). If the device is 32 bit
 * capable or less, then VFIO dev_map() reserves free unused IOVA address
 * (falling into 32bit address range) of requested size from IOVA allocator
 * (based on buddy system algorithm).The memory passed in sg_in is passed along
 * with reserved IOVA addresses to the VFIO framework through IOCTL commands.
 * The VFIO framework interacts with the IOMMU driver and maps the virtual
 * addresses into IOVA addresses (provided through IOVA allocator). These IOVA
 * addresses are copied into sg_in->io->physmap. These IOVA address can later
 * be used for DMA operations.
 * These below are the modules that needs to be inserted with the below
 * mentioned modparams into linux before using VFIO.
 *
 * insmod vfio.ko
 * insmod vfio_iommu_type1.ko allow_unsafe_interrupts=1
 * insmod vfio_virqfd.ko
 * insmod vfio-platform-base.ko
 * insmod vfio-platform.ko reset_required=0
 *
 * NOTE:
 * 1. To get VFIO working SMMU and CCI needs to be enabled in the design.
 * 2. The current implementation doesn't support Shared Memory allocated using
 *    ION/DMA-BUF framework.
 */

#include <metal/mutex.h>
#include <metal/alloc.h>
#include <metal/device.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <linux/vfio.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include "vfio.h"

/* Minimum allocation of page size is 4K */
#define MIN_ALLOC	(1 << 12)

/* Allow the maximum allocation of 2^32 == 4GB */
#define MAX_ALLOC	((unsigned long)1 << 32)

/*
 * The complete 4GB memory can be represented as a 1 block
 * of 4GB memory, 2 blocks of 2GB, 4 blocks of 1GB memory
 * and so on upto 2^20 blocks of 4KB memory. So, the total
 * memory can be represented from level[0]...level[20].The
 * level[0] has 1 block representing 4GB memory, level[1]
 * has 2 block with each block representing 2GB, so on upto
 * level[20] which has 2^20 blocks each representing 4KB.
 */
#define TOTAL_LEVELS	21

/* The base address from where the allocation should start */
#define START_ADDR	MIN_ALLOC

/* lock used during dma map/unmap */
static metal_mutex_t vfio_lock;

/* vfio private data */
struct vfio_pdata {
	/* container id */
	int container;
	/* group id */
	int group;
	/* device id */
	int device;
	/* number of irqs device has */
	int irq_num;
	/* the irq type info for each irq */
	void *irq_flags;
};

/* IOVA level meta data */
struct level_data {
	/* which level this data corresponds to */
	int level;
	/* pointer for storing the bitmap */
	char *ptr;
	/* total number of nodes in this level */
	int nodes;
};

/* The meta data for iova allocator */
static struct level_data ldata[TOTAL_LEVELS];

/* Set the irq info based on the index value */
static void set_irq_info(struct linux_device *ldev,
			 int index,
			 int val)
{
	if (ldev->device.irq_num > 1) {
		*(int *)((char *)ldev->device.irq_info + index * sizeof(int)) = val;
	} else {
		ldev->device.irq_info = (void *)(intptr_t)val;
	}
}

/* Get the irq info for the index value */
static int get_irq_info(struct linux_device *ldev,
			int index)
{
	int TmpVal;

	if (ldev->device.irq_num > 1) {
		TmpVal = *(int *)((char *)ldev->device.irq_info + index * sizeof(int));
	} else {
		TmpVal = (intptr_t)(ldev->device.irq_info);
	}

	return TmpVal;
}

/* Set the irq type flags for the given index */
static void set_irq_flags(struct linux_device *ldev,
			 int index,
			 int flags)
{
	struct vfio_pdata *pdata;

	pdata = (struct vfio_pdata *)ldev->priv_data;

	if (ldev->device.irq_num > 1) {
		*(int *)((char *)pdata->irq_flags + index * sizeof(int)) = flags;
	} else {
		pdata->irq_flags = (void *)(intptr_t)flags;
	}
}

/* Get the irq type flags for the given index */
static int get_irq_flags(struct linux_device *ldev,
			int index)
{
	int TmpVal;
	struct vfio_pdata *pdata;

	pdata = (struct vfio_pdata *)ldev->priv_data;

	if (ldev->device.irq_num > 1) {
		TmpVal = *(int *)((char *)pdata->irq_flags + index * sizeof(int));
	} else {
		TmpVal = (intptr_t)(pdata->irq_flags);
	}

	return TmpVal;
}

/* This checks whether the node index is free or not */
static bool node_is_busy(int node_index,
			int level)
{
	int base_index = 0;
	int offset = 0;
	int bit_map_offset = 0;
	struct level_data *lptr;

	/* Get the level metadata */
	lptr = &ldata[level];

	/* The base offset of the level the node is located in */
	base_index = (1 << level) - 1;

	/* Calculate bit field for the node index to be set as busy */
	offset = node_index - base_index;

	/* Calculate the bit map offset that needs to be set as busy */
	bit_map_offset = offset / 8;

	return !!(lptr->ptr[bit_map_offset] & (1 << (offset % 8)));
}

/* Clear the already set busy bit for the node index */
static void node_clear_busy(int node_index,
			    int level)
{
	int base_index = 0;
	int offset = 0;
	int bit_map_offset = 0;
	struct level_data *lptr;

	/* Get the level metadata */
	lptr = &ldata[level];

	/* The base offset of the level the node is located in */
	base_index = (1 << level) - 1;

	/* Calculate bit field for the node index to be set as busy */
	offset = node_index - base_index;

	/* Calculate the bit map offset that needs to be set as busy */
	bit_map_offset = offset / 8;

	/* Toggle the busy bit */
	lptr->ptr[bit_map_offset] &= ~(1 << (offset % 8));
}

/* Set the busy bit for the given node index */
static void node_set_busy(int node_index,
			  int level)
{
	int base_index = 0;
	int offset = 0;
	int bit_map_offset = 0;
	struct level_data *lptr;

	/* Get the level metadata */
	lptr = &ldata[level];

	/* The base offset of the level the node is located in */
	base_index = (1 << level) - 1;

	/* Calculate bit field for the node index to be set as busy */
	offset = node_index - base_index;

	/* Calculate the bit map offset that needs to be set as busy */
	bit_map_offset = offset / 8;

	/* Toggle the busy bit */
	lptr->ptr[bit_map_offset] |= (1 << (offset % 8));
}

/* This initializes the level meta data */
static void init_level_data(struct level_data *level_ptr,
			    int level)
{
	uint32_t size;

	/*
	 * Size of the bit map required to
	 * store busy/free info of the nodes.
	 */
	size = (1 << level) / 8;

	/* Atleast allocate 1 byte */
	if (size == 0)
		size = 1;

	/*
	 * Allocate memory for bit map used
	 * for identifying busy/free nodes.
	 */
	level_ptr->ptr = (void *)metal_allocate_memory(size);

	/*
	 * Clear the bit fields of the memory
	 * allocated(bit_map) to zero.
	 */
	memset(level_ptr->ptr, 0x00, size);

	/* Store the level number */
	level_ptr->level = level;

	/*
	 * Store the total number of nodes
	 * present in this level.
	 */
	level_ptr->nodes = 1 << level;
}


/* This call finds the correct level for the requested memory size */
static int get_level(unsigned long size)
{
	int i = 0;
	unsigned long node_size;

	for (i = (TOTAL_LEVELS - 1); i >= 0; i--) {

		/* Node size of that level = ( MAX_ALLOC / (1 << level) )*/
		node_size = (MAX_ALLOC >> i);

		if (node_size >= size)
			break;
	}

	if (i < 0) {
		metal_log(METAL_LOG_ERROR, "Failed to find level\n");
		return -EFAULT;
	}

	return i;
}

/* Returns the free node in a level requested */
static int get_free_node(int level)
{
	int i = 0;
	/* Total nodes in that level */
	int total_nodes = 1 << level;

	/* Start node index in that level */
	int node_base = total_nodes - 1;

	int node_index;

	for (i = 0; i < total_nodes; i ++) {
		node_index = node_base + i;
		if (!node_is_busy(node_index, level)) {
			/* Found a free node */
			return node_index;
		}
	}

	/* No free nodes */
	return -ENODEV;
}

/* Get the parent node index for a given node */
static int get_parent(int node_index)
{
	int node;

	if (node_index == 0) {
		/* We are at the top */
		node = 0;
	} else {
		node = (node_index - 1) / 2;
	}

	return node;
}

/* Get the left child node for a given node index */
static int get_lchild(int node_index)
{
	return ((node_index * 2) + 1);
}

/* Get the right child node for a given node index */
static int get_rchild(int node_index)
{
	return ((node_index * 2) + 2);
}

/* Get the buddy node for the given node index */
static int get_buddy_node(int node_index)
{
	int buddy;

	buddy = ((node_index + 1) ^ 1);

	buddy = buddy - 1;

	return buddy;
}

/* Set parent node as busy */
static void set_parent_busy(int node_index,
			    int level)
{
	int parent_level;
	int parent;

	/* Set busy flag for the node */
	node_set_busy(node_index, level);

	if ((node_index > 0) && (level > 0)) {
		parent_level = level - 1;

		parent = get_parent(node_index);

		/*
		 * Recursively set the parent nodes as
		 * busy (until we reach to the top)
		 */
		set_parent_busy(parent, parent_level);
	}

	return;
}

/* Set parent node as free */
static void set_parent_free(int node_index,
			    int level)
{
	int parent_level;
	int parent, buddy_index;

	/* Clear busy flag for the node */
	node_clear_busy(node_index, level);

	if ((node_index > 0) && (level > 0)) {

		/* Get buddy index */
		buddy_index = get_buddy_node(node_index);

		/* Do not proceed if our buddy is not free */
		if (node_is_busy(buddy_index, level)) {
			return;
		}

		parent_level = level - 1;

		parent = get_parent(node_index);

		/*
		 * Recursively set the parent nodes
		 * as free (until we reach to the top)
		 */
		set_parent_free(parent, parent_level);
	}

	return;
}

/* Set the left and right child of the node as busy */
static void set_child_busy(int node_index,
			   int level)
{
	int lchild, rchild;
	int child_level;

	/* Set busy flag for the node */
	node_set_busy(node_index, level);

	if (level < TOTAL_LEVELS - 1) {
		child_level = level + 1;

		/* Set busy flags for the left and right childs */
		lchild = get_lchild(node_index);
		rchild = get_rchild(node_index);

		/* Recursively set the child nodes as busy */
		set_child_busy(lchild, child_level);
		set_child_busy(rchild, child_level);
	}

	return;
}

/* Set the left and right child of the node as free */
static void set_child_free(int node_index,
			   int level)
{
	int lchild, rchild;
	int child_level;

	/* Clear busy flag for the node */
	node_clear_busy(node_index, level);

	if (level < TOTAL_LEVELS - 1) {
		child_level = level + 1;

		/* Clear busy flags for the left and right childs */
		lchild = get_lchild(node_index);
		rchild = get_rchild(node_index);

		/* Recursively clear the child nodes as free */
		set_child_free(lchild, child_level);
		set_child_free(rchild, child_level);
	}

	return;
}

/* Convert node index to address */
static unsigned long node_to_ptr(int node_index,
				 int level)
{
	int base_node;
	int offset;
	unsigned long node_size;
	unsigned long ptr;


	/* Start node number of that level */
	base_node = ((1 << level)  - 1);

	/* Offset of the requested node from the start node */
	offset = node_index - base_node;

	/* Node size of that level = ( MAX_ALLOC / (1 << level) )*/
	node_size = (MAX_ALLOC >> level);

	ptr = START_ADDR + offset * node_size;

	return ptr;
}

/* Convert address to node index */
static int ptr_to_node(void *ptr,
		       int level)
{
	unsigned long tmp_ptr;
	unsigned long node_size;
	int node_offset;
	int base_node;

	/* Node size of that level = ( MAX_ALLOC / (1 << level) )*/
	node_size = (MAX_ALLOC >> level);

	/* Start node number of that level */
	base_node = ((1 << level)  - 1);

	tmp_ptr = (unsigned long)ptr - START_ADDR;

	node_offset = tmp_ptr / node_size;

	node_offset = node_offset + base_node;

	return node_offset;
}

/* Free the iova address */
static void metal_iova_free(void *ptr,
			    unsigned long size)
{
	int level = -1;
	int node_index;
	int buddy_index;

	/* Get the level from size */
	level = get_level(size);
	if (level < 0)
		return;

	/* Get the node index */
	node_index = ptr_to_node(ptr, level);

	/* Clear the busy node */
	node_clear_busy(node_index, level);

	/* Get buddy index */
	buddy_index = get_buddy_node(node_index);

	if (!node_is_busy(buddy_index, level))
		set_parent_free(buddy_index, level);

	/* Clear the child busy bits */
	set_child_free(node_index, level);
}

/* Get the free iova address for the requested size */
static void *metal_iova_alloc(unsigned long size, int dma_cap)
{
	int level = -1;
	int free_node;
	void *address = NULL;

	/* Calculate the level for the requested size */
	level = get_level(size);
	if (level < 0)
		return NULL;

	/* Find free node in the level */
	free_node = get_free_node(level);

	if (free_node < 0) {
		/* Can't find a free node */
		address = NULL;
	} else {
		/* Need to set the node and all childs of the node as busy */
		set_child_busy(free_node, level);

		/* Need to set the parent as busy */
		set_parent_busy(free_node, level);

		/* Convert the node index to address */
		address = (void *)node_to_ptr(free_node, level);

		/* Check if device is dma capable for address generated */
		if (((unsigned long)address + size) >
			(((unsigned long)1 << dma_cap) - 1)) {
			metal_iova_free(address, size);
			return NULL;
		}
	}

	return address;
}

static int find_groupid(char *dev_ptr) {
	struct udev_enumerate *enumerate;
	struct udev_list_entry *list_entry;
	struct udev_device *device;
	struct udev *udev;
	const int flags = O_RDONLY;
	const int mode = S_IRUSR | S_IRGRP | S_IROTH;
	char path[PATH_MAX];
	int fd = -1;
	int ret;
	const char *str;

	udev = udev_new();
	enumerate = udev_enumerate_new(udev);

	udev_enumerate_add_match_subsystem(enumerate, "vfio");
	udev_enumerate_scan_devices(enumerate);

	udev_list_entry_foreach(list_entry,
				udev_enumerate_get_list_entry(enumerate)) {
		device = udev_device_new_from_syspath(udev_enumerate_get_udev(enumerate),
                                                      udev_list_entry_get_name(list_entry));

		str = udev_device_get_sysname(device);

		ret = snprintf(path, sizeof(path),
			       "/sys/kernel/iommu_groups/%s/devices/%s",
			       str, dev_ptr);
		if (ret >= (int)sizeof(path)) {
			metal_log(METAL_LOG_ERROR,
				  "%s: %d: path exceeding %d bytes\n",
				  __func__, __LINE__, sizeof(path));
			return -EOVERFLOW;
		}

		fd = open(path, flags, mode);
		if (fd < 0) {
			continue;
		} else {
			close(fd);
			udev_device_unref(device);
			udev_unref(udev);
			return atoi(str);
		}

		udev_device_unref(device);
	}

	udev_unref(udev);
	return -EINVAL;
}

/*
 * The function unbinds the device from any existing driver and binds
 * it to vfio-platform driver
 */
static int vfio_probe(struct linux_device *ldev,
		      struct linux_driver *ldrv)
{
	int group, ret;
	const char *drv_name;
	char command[PATH_MAX];
	char path[PATH_MAX];

	drv_name = udev_device_get_driver(ldev->udev_device);
	if (!drv_name || strcmp(drv_name, ldrv->drv_name)) {
		metal_log(METAL_LOG_WARNING,
			  "Bound to incompatible driver: %s expected: %s\n",
			  drv_name, ldrv->drv_name);

		if (drv_name != NULL ) {
			ret = snprintf(path, sizeof(path),
				       "/sys/bus/platform/devices/%s/driver",
				       ldev->dev_name);
			if (ret >= (int)sizeof(path)) {
				metal_log(METAL_LOG_ERROR,
					  "%s: %d: path exceeding %d bytes\n",
					  __func__, __LINE__, sizeof(path));
				return -EOVERFLOW;
			}

			if (!metal_check_file_available(path)) {
				ret = snprintf(command, sizeof(command),
						"%s/unbind", path);
				if (ret >= (int)sizeof(command)) {
					metal_log(METAL_LOG_ERROR,
						  "%s: %d: command exceeding %d bytes\n",
						  __func__, __LINE__,
						  sizeof(command));
					return -EOVERFLOW;
				}

				ret = metal_linux_exec_cmd(ldev->dev_name,
							   command);
				if (ret)
					return ret;
			}
		}

		ret = snprintf(command, sizeof(command),
				"/sys/bus/platform/devices/%s/driver_override",
				ldev->dev_name);
		if (ret >= (int)sizeof(command)) {
			metal_log(METAL_LOG_ERROR,
				  "%s: %d: command exceeding %d bytes\n",
				  __func__, __LINE__, sizeof(command));
			return -EOVERFLOW;
		}

		ret = metal_linux_exec_cmd(ldrv->drv_name, command);
		if (ret)
			return ret;

		ret = snprintf(command, sizeof(command),
				"/sys/bus/platform/drivers_probe");
		if (ret >= (int)sizeof(command)) {
			metal_log(METAL_LOG_ERROR,
				  "%s: %d: command exceeding %d bytes\n",
				  __func__, __LINE__, sizeof(command));
			return -EOVERFLOW;
		}

		ret = metal_linux_exec_cmd(ldev->dev_name, command);
		if (ret)
			return ret;
	}

	group = find_groupid(ldev->dev_name);
	if (group < 0) {
		metal_log(METAL_LOG_ERROR,
			  "Failed to get groupid for device: %s\n",
			  ldev->dev_name);
		return -ENODEV;
	} else {
		metal_log(METAL_LOG_DEBUG,
			  "Got group: %d for device: %s\n", group,
			  ldev->dev_name);
	}

	return group;
}

/*
 * This function checks if device is VFIO compatible and intializes the
 * device related information got through vfio framework
 */
static int vfio_init(int group_id, struct linux_device *ldev)
{
	int ret;
	void *virt;
	int container, group, device;
	struct metal_io_region *io;
	struct vfio_pdata *pdata;
	const char *vfio_container = "/dev/vfio/vfio";
	const char *dev_vfio = "/dev/vfio";
	char group_path[PATH_MAX];
	struct vfio_group_status group_status =
					{ .argsz = sizeof(group_status) };
	struct vfio_iommu_type1_info iommu_info =
					{ .argsz = sizeof(iommu_info) };
	struct vfio_device_info device_info =
					{ .argsz = sizeof(device_info) };
	struct vfio_region_info region_info =
					{ .argsz = sizeof(region_info) };

	/* Get the group path of the device */
	snprintf(group_path, sizeof(group_path), "%s/%d", dev_vfio, group_id);

	/* Create a new container */
	container = open(vfio_container, O_RDWR);
	if (ioctl(container, VFIO_GET_API_VERSION) != VFIO_API_VERSION) {
		metal_log(METAL_LOG_ERROR, "Unknown API version\n");
		return -ENODEV;
	}

	/* Check if VFIO device driver supports type1 IOMMU */
	if (!ioctl(container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU)) {
		metal_log(METAL_LOG_ERROR,
			  "Doesn't support the IOMMU driver we want\n");
		return -ENODEV;
	}

	/* Open the group */
	group = open(group_path, O_RDWR);

	/* Test the group is viable and available */
	ioctl(group, VFIO_GROUP_GET_STATUS, &group_status);

	if (!(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		metal_log(METAL_LOG_ERROR, "Group is not viable\n");
		return -ENODEV;
	}

	/* Add the group to the container */
	ioctl(group, VFIO_GROUP_SET_CONTAINER, &container);

	/* Enable the IOMMU model we want */
	ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU);

	/* Get addition IOMMU info */
	ioctl(container, VFIO_IOMMU_GET_INFO, &iommu_info);

	/* Get a file descriptor for the device */
	device = ioctl(group, VFIO_GROUP_GET_DEVICE_FD, ldev->dev_name);

	/* Test and setup the device */
	ret = ioctl(device, VFIO_DEVICE_GET_INFO, &device_info);
	if(ret) {
		metal_log(METAL_LOG_ERROR, "Could not get VFIO device info\n");
		return -ENODEV;
	}

	/* Store the container for future usage */
	ldev->fd = container;

	/* Allocate memory for vfio private data */
	pdata = (struct vfio_pdata *)metal_allocate_memory(sizeof(struct vfio_pdata));

	/* Initialize the vfio private data for future usage */
	pdata->group = group;
	pdata->container = container;
	pdata->device = device;

	metal_log(METAL_LOG_DEBUG, "num_regions=%d num_irqs=%d\n",
		  device_info.num_regions, device_info.num_irqs);

	memset(ldev->device.regions, 0,
	       sizeof(struct metal_io_region) * METAL_MAX_DEVICE_REGIONS);

	for (uint32_t i =0; ((i < device_info.num_regions) &&
	     (i < METAL_MAX_DEVICE_REGIONS)); i++) {
		region_info.index = i;
		ret = ioctl(device, VFIO_DEVICE_GET_REGION_INFO, &region_info);
		if(ret) {
			metal_log(METAL_LOG_ERROR,
				  "Could not get VFIO region info : %d\n", i);
			return -ENODEV;
		}

		virt = (void *)mmap(NULL, region_info.size,
				    PROT_READ | PROT_WRITE, MAP_SHARED, device,
				    region_info.offset);
		if (!virt) {
			metal_log(METAL_LOG_ERROR, "Failed to mmap\n");
			return -ENOMEM;
		}

		metal_log(METAL_LOG_DEBUG, "offset: 0x%llx size: 0x%llx\n",
			  region_info.offset, region_info.size);

		io = &ldev->device.regions[ldev->device.num_regions];
		metal_io_init(io, virt, 0x00, region_info.size, -1, 0, NULL);
		ldev->device.num_regions++;
	}

	/* Populate the total number of irqs available for future usage */
	ldev->device.irq_num =  device_info.num_irqs;

	/*
	 * If irq_num > 1, then allocate memory for storing irq_info and
	 * irq_flags. When irq_num == 1, the directly device.irq_info and
	 * device.irq_flags for storing information.
	 */
	if (ldev->device.irq_num > 1) {
		ldev->device.irq_info =
			(void *)malloc(ldev->device.irq_num * sizeof(int));
		pdata->irq_flags =
			(void *)malloc(ldev->device.irq_num * sizeof(int));
	}

	/* Store the private data in struct linux_device */
	metal_device_set_pdata(ldev, (void *)pdata);

	for (unsigned int i = 0; i < device_info.num_irqs; i ++) {
		struct vfio_irq_info irq_info =
					{ .argsz = sizeof(irq_info) };
		int size = sizeof(struct vfio_irq_set) + sizeof(int);
		struct vfio_irq_set *irq_set =
					(struct vfio_irq_set *)malloc(size);
		int event_fd = eventfd(0, 0);
		int *efd_ptr;

		/* Set the irq index */
		irq_info.index = i;

		/* Get the irq information */
		ret = ioctl(device, VFIO_DEVICE_GET_IRQ_INFO, &irq_info);
		if(ret) {
			metal_log(METAL_LOG_ERROR,
				  "Could not get VFIO device irq info\n");
			return -ENODEV;
		}

		metal_log(METAL_LOG_DEBUG, "index=0x%x flags=0x%x count=0x%x\n",
			  irq_info.index, irq_info.flags, irq_info.count);

		memset(irq_set, 0, size);

		/*
		 * Populate irq_set for communicating with vfio-platform
		 * driver for registering eventfd on irq number represented
		 * using irq_info.index
		 */
		irq_set->argsz = size;
		irq_set->index = irq_info.index;
		irq_set->start = 0;
		irq_set->flags = VFIO_IRQ_SET_DATA_EVENTFD |
				 VFIO_IRQ_SET_ACTION_TRIGGER;
		irq_set->count = 1;
		efd_ptr = (int *)&irq_set->data;
		*efd_ptr = event_fd;

		ret = ioctl(device, VFIO_DEVICE_SET_IRQS, irq_set);
		if(ret) {
			metal_log(METAL_LOG_ERROR,
					"Could not set VFIO device irq\n");
			return -ENODEV;
		}

		/* Set the eventfd for the irq */
		set_irq_info(ldev, i, event_fd);

		/* Set the irq type flags for the irq */
		set_irq_flags(ldev, i, irq_info.flags);

		metal_linux_irq_register_dev(&ldev->device, event_fd);
	}

	return 0;
}

/*
 * This function binds the device to vfio-platform driver and
 * initializes the device related information
 */
static int metal_vfio_dev_bind(struct linux_device *ldev,
		struct linux_driver *ldrv)
{
	int group, ret;

	group = vfio_probe(ldev, ldrv);
	if (group < 0) {
		metal_log(METAL_LOG_WARNING, "Failed to find group\n");
		return -ENODEV;
	}

	ret = vfio_init(group, ldev);
	if (ret < 0) {
		metal_log(METAL_LOG_ERROR, "Failed to vfio_init\n");
		return -ENODEV;
	}

	metal_log(METAL_LOG_DEBUG, "bound device %s to driver %s\n",
		  ldev->dev_name, ldrv->drv_name);

	return 0;
}

/* This function unbinds the device from vfio-platform driver */
static int metal_vfio_dev_unbind(struct linux_device *ldev) {
	struct vfio_pdata *pdata;

	if (!ldev->priv_data)
		return 0;

	pdata = (struct vfio_pdata *)ldev->priv_data;

	/* Close the prevoiusly opened device fd */
	if(close(pdata->device)) {
		metal_log(METAL_LOG_ERROR, "Failed to close device: %d\n",
			  pdata->device);
		return -ENODEV;
	}

	/* Unset the group from the container */
	if(ioctl(pdata->group, VFIO_GROUP_UNSET_CONTAINER, &pdata->container)) {
		metal_log(METAL_LOG_ERROR,
			  "Failed to unset group from container\n");
		return -ENODEV;
	}

	/* Close the container */
	if (close(pdata->container)) {
		metal_log(METAL_LOG_ERROR, "Failed to close container: %d\n",
			  pdata->container);
		return -ENODEV;
	}

	/* Close the group */
	if (close(pdata->group)) {
		metal_log(METAL_LOG_ERROR, "Failed to close group: %d\n",
			  pdata->group);
		return -ENODEV;
	}

	/* Freeing the pdata */
	free(pdata);

	ldev->priv_data = NULL;

	return 0;
}

/* Dma map an address using vfio framework */
static metal_phys_addr_t vfio_dma_map(struct linux_device *ldev,
				      struct metal_sg *sg)
{
	int ret, i = 0;
	int dma_cap;
	struct metal_io_region *io;
	unsigned long iova = 0;
	struct vfio_iommu_type1_dma_map ptr;

	memset(&ptr, 0, sizeof(struct vfio_iommu_type1_dma_map));
	ptr.argsz = sizeof(struct vfio_iommu_type1_dma_map);

	/* Acquire the lock */
	metal_mutex_acquire(&vfio_lock);

	dma_cap = metal_device_get_dmacap(&ldev->device);
	if (dma_cap < 64) {
		/*
		 * Allocate free iova address from IOVA allocator
		 * that fits the sg->len size.
		 */
		iova = (unsigned long)metal_iova_alloc(sg->len, dma_cap);
	} else {
		/*
		 * Device dma has 64 bit addressing capability.
		 * Directly use the Virtual Address for IOVA address.
		 */
		iova = (unsigned long)sg->virt;
	}

	if (!iova) {
		metal_log(METAL_LOG_ERROR,
		  "%s: VFIO: Failed to reserved iova address for len: %d\n",
		  ldev->dev_name, sg->len);
		goto EXIT;
	}

	metal_log(METAL_LOG_DEBUG,
		  "%s: VFIO: Reserved iova = 0x%llx len: %d\n",
		  ldev->dev_name, iova, sg->len);

	ptr.vaddr = (unsigned long)sg->virt;
	ptr.size = (unsigned long)sg->len;
	ptr.iova = iova;
	ptr.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE;

	ret = ioctl(ldev->fd, VFIO_IOMMU_MAP_DMA, &ptr);
	if(ret) {
		metal_log(METAL_LOG_ERROR,
			  "%s: VFIO:Failed to DMA MAP address: 0x%llx ret=%d\n",
			  ldev->dev_name, sg->virt, ret);
		iova = (metal_phys_addr_t) 0;
		goto EXIT;
	}

	for (i = 0; i < METAL_MAX_DEVICE_REGIONS; i ++) {
		io = &ldev->device.regions[i];

		if (!io->virt) {
			ldev->region_phys[i] = (metal_phys_addr_t)iova;
			metal_io_init(io, (void *)ptr.vaddr,
				      &ldev->region_phys[i], (size_t)ptr.size,
				      -1, 0, NULL);
			break;
		}
	}

	if (i >= METAL_MAX_DEVICE_REGIONS) {
		metal_log(METAL_LOG_ERROR,
			  "%s: VFIO:number of io regions exhausted\n",
			  ldev->dev_name);
		ioctl(ldev->fd, VFIO_IOMMU_UNMAP_DMA, &ptr);
		iova = 0;
		goto EXIT;
	}

	/* Update the io contents in sg */
	if (sg->io == NULL)
		sg->io = io;
	else
		memcpy(sg->io, io, sizeof(struct metal_io_region));

EXIT:
	/* Release the lock */
	metal_mutex_release(&vfio_lock);

	return iova;
}

/* Unmap the previously mapped address */
static void vfio_dma_unmap(struct linux_device *ldev,
			   struct metal_sg *sg)
{
	int ret;
	int size;
	struct metal_io_region *io;
	struct vfio_iommu_type1_dma_unmap *ptr;

	size = sizeof(struct vfio_iommu_type1_dma_unmap);
	ptr = (struct vfio_iommu_type1_dma_unmap *)metal_allocate_memory(size);

	/* Acquire the lock */
	metal_mutex_acquire(&vfio_lock);

	ptr->argsz = size;

	ptr->size = (unsigned long)sg->len;
	ptr->flags = 0;

	if (sg->io->physmap) {
		ptr->iova = (unsigned long)*sg->io->physmap;
		sg->io->physmap = (metal_phys_addr_t *)0;
	} else {
		if (!sg->io->virt) {
			metal_log(METAL_LOG_ERROR,
				  "%s:VFIO:Failed to UNMAP. No valid sg->io->virt address\n",
				  ldev->dev_name);
			goto EXIT;
		}

		for (int i = 0; i < METAL_MAX_DEVICE_REGIONS; i ++) {
			io = &ldev->device.regions[i];

			if (sg->io->virt == io->virt) {
				ptr->iova =  (unsigned long)*io->physmap;
				break;
			}
		}
	}

	ret = ioctl(ldev->fd, VFIO_IOMMU_UNMAP_DMA, ptr);
	if(ret) {
		metal_log(METAL_LOG_ERROR,
			  "%s:VFIO:Failed to UNMAP iova:0x%llx len=%d ret=%d\n",
			  ldev->dev_name, ptr->iova, ptr->size, ret);
		goto EXIT;
	}

	/*
	 * Free the previously reserved IOVA address (for devices
	 * less than 64 bit addressing capability)
	 */
	if (metal_device_get_dmacap(&ldev->device) < 64)
		metal_iova_free((void *)ptr->iova, sg->len);

	/* Error out if METAL_MAX_DEVICE_REGIONS are full */
	for (int i = 0; i < METAL_MAX_DEVICE_REGIONS; i ++) {
		io = &ldev->device.regions[i];

		if (sg->io->virt == io->virt) {
			io->virt = NULL;
			io->size = 0;
			ldev->region_phys[i] = 0;
			break;
		}
	}

EXIT:
	/* Release the lock */
	metal_mutex_release(&vfio_lock);

	return;
}

/* Initialize the iova allocator */
void metal_iova_init(void) {
	int i = 0;

	/* Initialize mutex lock used at the time of IOVA address allocation */
	metal_mutex_init(&vfio_lock);

	for (i = 0; i < TOTAL_LEVELS; i++) {
		init_level_data(&ldata[i], i);
	}
}

void metal_vfio_dev_close(struct linux_bus *lbus,
			  struct linux_device *ldev)
{
	struct metal_io_region *io;
	char command[PATH_MAX];
	char path[PATH_MAX];
	int ret;

	(void)lbus;

	for (uint32_t i = 0; (i < ldev->device.num_regions); i ++) {
		io = &ldev->device.regions[i];
		munmap(io->virt, io->size);
	}

	metal_vfio_dev_unbind(ldev);

	if (ldev->udev_device) {
		udev_device_unref(ldev->udev_device);
		udev_unref(ldev->udev);
		ldev->udev_device = NULL;
		ldev->udev = NULL;
	}

	ret = snprintf(path, sizeof(path),
		       "/sys/bus/platform/devices/%s/driver",
		       ldev->dev_name);
	if (ret >= (int)sizeof(path)) {
		metal_log(METAL_LOG_ERROR,
			  "%s: %d: path exceeding %d bytes\n",
			  __func__, __LINE__, sizeof(path));
		return;
	}

	if (!metal_check_file_available(path)) {
		ret = snprintf(command, sizeof(command),
			       "%s/unbind", path);
		if (ret >= (int)sizeof(command)) {
			metal_log(METAL_LOG_ERROR,
				  "%s: %d: command exceeding %d bytes\n",
				  __func__, __LINE__, sizeof(command));
			return;
		}

		metal_linux_exec_cmd(ldev->dev_name, command);
	}

	ret = snprintf(command, sizeof(command),
		       "/sys/bus/platform/devices/%s/driver_override",
		       ldev->dev_name);
	if (ret >= (int)sizeof(command)) {
		metal_log(METAL_LOG_ERROR,
			  "%s: %d: command exceeding %d bytes\n",
			  __func__, __LINE__, sizeof(command));
		return;
	}

	metal_linux_exec_cmd(" ", command);
}

int metal_vfio_dev_open(struct linux_bus *lbus,
			struct linux_device *ldev)
{
	struct linux_driver *ldrv = ldev->ldrv;
	const char *syspath_ptr;
	int result;

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
		return -EOVERFLOW;
	}

	result = metal_vfio_dev_bind(ldev, ldrv);
	if (result) {
		if (ldev->udev_device) {
			udev_device_unref(ldev->udev_device);
			udev_unref(ldev->udev);
			ldev->udev_device = NULL;
			ldev->udev = NULL;
		}

		return result;
	}

	/* Set the default dma capability to 64 bit */
	metal_device_set_dmacap(&ldev->device, 64);

	metal_log(METAL_LOG_DEBUG, "opened %s:%s\n",
		  lbus->bus_name, ldev->dev_name);

	return 0;
}

void metal_vfio_dev_irq_ack(struct linux_bus *lbus,
			    struct linux_device *ldev,
			    int irq)

{
	int ret;
	int irq_flags;
	int index = -1;
	uint64_t val;
	struct vfio_irq_set irq_set = { .argsz = sizeof(struct vfio_irq_set) };
	struct vfio_pdata *pdata;

	(void)lbus;

	pdata = (struct vfio_pdata *)ldev->priv_data;

	ret = read(irq, (void *)&val, sizeof(val));
	if (ret < 0) {
		metal_log(METAL_LOG_ERROR,
			  "%s, read vfio irq fd %d failed: %d.\n",
			  __func__, irq, ret);
		return;
	}

	/* Find valid irq index */
	for (int i = 0, TmpVal; i < ldev->device.irq_num; i++) {
		TmpVal = get_irq_info(ldev, i);
		if (TmpVal == irq) {
			index = i;
			break;
		}
	}

	if (index == -1 ) {
		metal_log(METAL_LOG_ERROR,
			  "Could not find VFIO index for irq = %d\n", irq);
		return;
	}

	irq_flags = get_irq_flags(ldev, index);

	/* Unmask the irq, which got automatically masked by vfio irq handler */
	if (irq_flags & VFIO_IRQ_INFO_AUTOMASKED) {
		irq_set.index = index;
		irq_set.start = 0;
		irq_set.flags = VFIO_IRQ_SET_DATA_NONE |
				VFIO_IRQ_SET_ACTION_UNMASK;
		irq_set.count = 1;

		ret = ioctl(pdata->device, VFIO_DEVICE_SET_IRQS, irq_set);
		if(ret) {
			metal_log(METAL_LOG_ERROR,
					"Could not set VFIO device irq\n");
			return;
		}
	}
}

int metal_vfio_dev_dma_map(struct linux_bus *lbus,
			   struct linux_device *ldev,
			   uint32_t dir,
			   struct metal_sg *sg_in,
			   int nents_in,
			   struct metal_sg *sg_out)
{
	int i;

	(void)lbus;
	(void)dir;

	for (i = 0; i < nents_in; i++) {
		if (!vfio_dma_map(ldev, &sg_in[i]))
			return -EINVAL;
	}

	if (sg_in != sg_out)
		memcpy(sg_out, sg_in, nents_in*(sizeof(struct metal_sg)));

	return nents_in;
}

void metal_vfio_dev_dma_unmap(struct linux_bus *lbus,
			      struct linux_device *ldev,
			      uint32_t dir,
			      struct metal_sg *sg,
			      int nents)
{
	int i;
	(void) lbus;
	(void) ldev;
	(void) dir;

	for (i = 0; i < nents; i++) {
		vfio_dma_unmap(ldev, &sg[i]);
	}

	return;
}
