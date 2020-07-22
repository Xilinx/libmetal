/*
 * This example does the below
 * 1. Creates source and destination buffers
 * 2. Fill source buffer with random data
 * 3. Program the AXI DMA IP in PL with the source and dest buffer address
 * 4. Start the AXI DMA to send the data in source buffer to dest buffer
 * 5. Once transfer is done, verifies the source and destination buffer data
 *    are equal.
 *
 * Usage: vfio_axidma_demo <Address of AXI DMA IP>
 *
 * Note: This example is depending on the AXI DMA IP(simple mode) added into PL
 */
#include <metal/sys.h>
#include <metal/device.h>
#include <metal/log.h>
#include <metal/scatterlist.h>

typedef __u8 uchar;
typedef __u32 uint;
typedef __u32 u32;
typedef __u64 u64;

#define XAxiDma_Out32			metal_io_write32
#define XAxiDma_In32			metal_io_read32

#define XAXIDMA_TEST_SIZE		8192        /* Test Size */
#define XAXIDMA_DMA_CAP			32          /* DMA capability */
#define XAXIDMA_MM2S_CR_OFFSET    	0x00000000  /* Control register */
#define XAXIDMA_MM2S_SRCADDR_OFFSET 	0x00000018  /* Source address register */
#define XAXIDMA_MM2S_SRCADDR_MSB 	0x0000001C  /* Source address MSB register */
#define XAXIDMA_MM2S_BTT_OFFSET     	0x00000028  /* Bytes to transfer */
#define XAXIDMA_S2MM_CR_OFFSET    	0x00000030  /* Control register */
#define XAXIDMA_S2MM_SR_OFFSET    	0x00000034  /* Status register */
#define XAXIDMA_S2MM_SRCADDR_OFFSET 	0x00000048  /* Source address register */
#define XAXIDMA_S2MM_SRCADDR_MSB 	0x0000004C  /* Source address MSB register */
#define XAXIDMA_S2MM_BTT_OFFSET     	0x00000058  /* Bytes to transfer */
#define XAXIDMA_CR_RS_MASK		0x00000001  /* R/S bit */
#define XAXIDMA_CR_RESET_MASK	        0x00000004  /* Reset DMA engine */
#define XAXIDMA_XR_IRQ_IOC_MASK		0x00001000  /* Completion interrupt */
#define XAXIDMA_XR_IRQ_ALL_MASK		0x00007000  /* All interrupts */

static unsigned long __XAxiDma_MetalMap(struct metal_device *DevicePtr,
					void *vaddr,
					int size)
{
	struct metal_sg sg_out;
	struct metal_sg sg_in;
	int ret;

	memset(&sg_out, 0, sizeof(struct metal_sg));
	memset(&sg_in, 0, sizeof(struct metal_sg));

	sg_in.virt = vaddr;
	sg_in.len = size;
	sg_in.io = NULL;

	ret = metal_dma_map(DevicePtr, METAL_DMA_DEV_WR, &sg_in, 1, &sg_out);
	if (ret < 0) {
		printf("Failed to dma map addr: 0x%lx size: %d ret: %d\n",
		       (unsigned long)vaddr, size, ret);
		return 0;
	}

	metal_device_add_ioregion(DevicePtr, sg_in.io);

	return (unsigned long)*sg_in.io->physmap;
}

static void *XAxiDma_MetalMap(struct metal_device *device,
			      unsigned long *phys,
			      int size)
{
	void *va;

	va = aligned_alloc(4096, size);
	*phys = (unsigned long)__XAxiDma_MetalMap(device, va, size);

	return va;
}

static void XAxiDma_MetalUnmap(struct metal_device *DevicePtr,
			       void *virt,
			       int size)
{
	struct metal_io_region io;
	struct metal_sg sg;

	memset(&io, 0, sizeof(struct metal_io_region));
	memset(&sg, 0, sizeof(struct metal_sg));

	io.physmap = (metal_phys_addr_t)0;
	io.virt = (void *)virt;
	sg.virt = (void *)virt;
	sg.len = size;
	sg.io = &io;

	metal_dma_unmap(DevicePtr, METAL_DMA_DEV_WR, &sg, 1);

	return;
}

/* Send the data from Source buffer to Destination buffer */
int XAxiDma_SendData(struct metal_io_region *io,
		     uintptr_t SrcBufferPhysPtr,
		     uintptr_t DstBufferPhysPtr,
		     int timeout_secs)
{
	struct timespec start, finish;
	volatile u32 RegValue;
	volatile u32 ResetMask;

	memset((char *)&start, 0, sizeof(struct timespec));
	memset((char *)&finish, 0, sizeof(struct timespec));

	/* Reset the MM2S channel */
	clock_gettime(CLOCK_REALTIME, &start);
	do {
		ResetMask = (unsigned) XAXIDMA_CR_RESET_MASK;
		XAxiDma_Out32(io, XAXIDMA_MM2S_CR_OFFSET, ResetMask);
		/* If the reset bit is still high, then reset is not done */
		ResetMask = XAxiDma_In32(io, XAXIDMA_MM2S_CR_OFFSET);
		if (!(ResetMask & XAXIDMA_CR_RESET_MASK)) {
			break;
		}
		clock_gettime(CLOCK_REALTIME, &finish);
	} while ((finish.tv_sec - start.tv_sec) < timeout_secs);

	if ((finish.tv_sec  - start.tv_sec) > timeout_secs)
		return -ETIMEDOUT;

	/* Reset the S2MM channel */
	clock_gettime(CLOCK_REALTIME, &start);
	do {
		ResetMask = (unsigned) XAXIDMA_CR_RESET_MASK;
		XAxiDma_Out32(io, XAXIDMA_S2MM_CR_OFFSET, ResetMask);

		/* If the reset bit is still high, then reset is not done */
		ResetMask = XAxiDma_In32(io, XAXIDMA_S2MM_CR_OFFSET);
		if (!(ResetMask & XAXIDMA_CR_RESET_MASK)) {
			break;
		}

		clock_gettime(CLOCK_REALTIME, &finish);
	} while ((finish.tv_sec  - start.tv_sec) < timeout_secs);

	if ((finish.tv_sec  - start.tv_sec) > timeout_secs)
		return -ETIMEDOUT;

	/* Start the DMA */
	RegValue = XAxiDma_In32(io, XAXIDMA_S2MM_CR_OFFSET);
	RegValue = (unsigned) (RegValue | XAXIDMA_CR_RS_MASK);
	XAxiDma_Out32(io, XAXIDMA_S2MM_CR_OFFSET, RegValue);

	/* Enable Interrupts */
	RegValue = XAxiDma_In32(io, XAXIDMA_S2MM_CR_OFFSET);
	RegValue = (unsigned) (RegValue | (0x1 << 12));
	XAxiDma_Out32(io, XAXIDMA_S2MM_CR_OFFSET, RegValue);

	/* Set the Dst buffer address */
	XAxiDma_Out32(io, XAXIDMA_S2MM_SRCADDR_OFFSET,
		      DstBufferPhysPtr & (((unsigned long)1 << 32) - 1));
	XAxiDma_Out32(io, XAXIDMA_S2MM_SRCADDR_MSB,
		      (unsigned long)DstBufferPhysPtr >> 32);

	/* Set the total number of bytes to recieve */
	XAxiDma_Out32(io, XAXIDMA_S2MM_BTT_OFFSET, XAXIDMA_TEST_SIZE);

	/* Start the DMA */
	RegValue = XAxiDma_In32(io, XAXIDMA_MM2S_CR_OFFSET);
	RegValue = (unsigned) (RegValue | XAXIDMA_CR_RS_MASK);
	XAxiDma_Out32(io, XAXIDMA_MM2S_CR_OFFSET, RegValue);

	/* Enable Interrupts */
	RegValue = XAxiDma_In32(io, XAXIDMA_MM2S_CR_OFFSET);
	RegValue = (unsigned) (RegValue | (0x1 << 12));
	XAxiDma_Out32(io, XAXIDMA_MM2S_CR_OFFSET, RegValue);

	/* Set the Src buffer address */
	XAxiDma_Out32(io, XAXIDMA_MM2S_SRCADDR_OFFSET,
		      SrcBufferPhysPtr & (((unsigned long)1 << 32) - 1));
	XAxiDma_Out32(io, XAXIDMA_MM2S_SRCADDR_MSB,
		      (unsigned long)SrcBufferPhysPtr >> 32);

	/* Write the total number of bytes to send */

	XAxiDma_Out32(io, XAXIDMA_MM2S_BTT_OFFSET, XAXIDMA_TEST_SIZE);

	/* Wait for transfer to get completed */
	clock_gettime(CLOCK_REALTIME, &start);
	do {
		RegValue = XAxiDma_In32(io, XAXIDMA_S2MM_SR_OFFSET);
	} while (!(RegValue & (0x1 << 12)) &&
			((finish.tv_sec  - start.tv_sec) < timeout_secs));

	if ((finish.tv_sec  - start.tv_sec) > timeout_secs)
		return -ETIMEDOUT;

	if ((RegValue & XAXIDMA_XR_IRQ_IOC_MASK)) {
		metal_log(METAL_LOG_DEBUG, "Transfer Completed \n ");
	}

	return 0;
}

/*
 * This function demonstrates the vfio support in libmetal using AXI DMA IP.
 * This function does the below
 * 1. Create memory buffers of XAXIDMA_TEST_SIZE for source and destination
 * 2. Populate random data into source buffer
 * 3. Send the data in source buffer to destination buffer using AXI DMA
 * 4. Compare the source and Destination buffers
 */
int XAxiDma_Test(struct metal_device *DevicePtr,
		 struct metal_io_region *io)
{
	uintptr_t SrcBufferPhysPtr, DstBufferPhysPtr;
	uintptr_t SrcBufferPtr, DstBufferPtr;

	/* Dev map the source buffer */
	SrcBufferPtr = (uintptr_t)XAxiDma_MetalMap(DevicePtr,
						   (void *)&SrcBufferPhysPtr,
						   XAXIDMA_TEST_SIZE);
	if (!SrcBufferPhysPtr || !SrcBufferPtr) {
		metal_log(METAL_LOG_ERROR,
			  "%s: %d: Failed to XAxiDma_MetalMap\n",
			  __func__, __LINE__);
		return -EINVAL;
	}

	/* Dev map the destination buffer */
	DstBufferPtr = (uintptr_t)XAxiDma_MetalMap(DevicePtr,
						   (void *)&DstBufferPhysPtr,
						   XAXIDMA_TEST_SIZE);
	if (!DstBufferPhysPtr || !DstBufferPtr) {
		metal_log(METAL_LOG_ERROR,
			  "%s: %d: Failed to XAxiDma_MetalMap\n",
			  __func__, __LINE__);
		return -EINVAL;
	}

	/* Populate random data into src buffer */
	for(int i = 0; i < XAXIDMA_TEST_SIZE; i++) {
		*((char *)SrcBufferPtr + i) = (rand() % 255);
	}

	/* Send the data from source to destination buffer */
	if (XAxiDma_SendData(io, SrcBufferPhysPtr, DstBufferPhysPtr, 10) < 0) {
		metal_log(METAL_LOG_ERROR, "Failed to transfer the data\n");
		return -EINVAL;
	}

	/* Compare the data */
	for (int i = 0; i < XAXIDMA_TEST_SIZE; i++) {
		if ((*((char *)SrcBufferPtr + i)) !=
				(*((char *)DstBufferPtr + i))) {
			metal_log(METAL_LOG_ERROR, "Expected = 0x%x Got = 0x%x",
				  *((char *)SrcBufferPtr + i),
				  *((char *)DstBufferPtr + i));
			return -EINVAL;
		}
	}

	/* Unmap the dev mapped source buffer */
	XAxiDma_MetalUnmap(DevicePtr,
			   (void *)SrcBufferPtr,
			   XAXIDMA_TEST_SIZE);

	/* Unmap the dev mapped destination buffer */
	XAxiDma_MetalUnmap(DevicePtr,
			   (void *)DstBufferPtr,
			   XAXIDMA_TEST_SIZE);

	return 0;
}

/* This function registers the AXI DMA device with libmetal */
int XAxiDma_RegisterMetal(struct metal_io_region **io,
			  uintptr_t Baseaddr,
			  struct metal_device **DevicePtr)
{
	int Status;
	char DevName[256];

	memset(DevName, 0, 256);

	/* Get device name based on the base address */
	Status = metal_devname_from_addr(Baseaddr, DevName);
	if (Status < 0) {
		metal_log(METAL_LOG_ERROR,
			  "Failed to find device name by id ret : %d\n",
			  Status);
		return -EINVAL;
	}

	/* Open the device */
	Status = metal_device_open("platform", DevName, DevicePtr);
	if (Status) {
		metal_log(METAL_LOG_ERROR,
			  "\n Failed to open device %s\n", DevName);
		return -EINVAL;
	}

	/* Set DMA addressing capability */
	metal_device_set_dmacap(*DevicePtr, XAXIDMA_DMA_CAP);

	/* Map AxiDma device IO region */
	*io = metal_device_io_region(*DevicePtr, 0);
	if (*io == NULL) {
		metal_log(METAL_LOG_ERROR,
			  "\n Failed to map region for %s.\n",
			  (*DevicePtr)->name);
		return -EINVAL;
	}

	return 0;
}

int main(int argc, char **argv)
{
	int Ret;
	struct metal_io_region *io;
	struct metal_device *DevicePtr = NULL;
	struct metal_init_params init_param = METAL_INIT_DEFAULTS;
	u32 BaseAddr;
	init_param.log_level = METAL_LOG_INFO;

	/* Initialize the libmetal */
	if (metal_init(&init_param)) {
		metal_log(METAL_LOG_ERROR,
			  "ERROR: Failed to run metal initialization\n");
		return -EINVAL;
	}

	if (argc < 2) {
		/* Print the usage */
		metal_log(METAL_LOG_ERROR,
			  "Enter valid AXI DMA base address\n\
			   usage: %s <base address>\n", argv[0]);
		return -EINVAL;
	}

	/* Get the Base Address of the AXI DMA IP */
	BaseAddr = strtoull(argv[1], NULL, 0);

	/* Register the AXI DMA device with base address */
	Ret = XAxiDma_RegisterMetal(&io, BaseAddr, &DevicePtr);
	if (Ret < 0) {
		metal_log(METAL_LOG_ERROR,
			  "Libmetal register failed %d\r\n", Ret);
		return -EINVAL;
	}

	/* Start the test */
	Ret = XAxiDma_Test(DevicePtr, io);
	if (Ret < 0) {
		metal_log(METAL_LOG_INFO, "AXIDMA TEST FAIL\n");
		return Ret;
	} else {
		metal_log(METAL_LOG_INFO, "AXIDMA TEST PASS\n");
		return 0;
	}
}
