#ifndef _UAPI_MSM_NPU_H_
#define _UAPI_MSM_NPU_H_

/* -------------------------------------------------------------------------
 * Includes
 * -------------------------------------------------------------------------
 */
#include <linux/types.h>

/* -------------------------------------------------------------------------
 * Defines
 * -------------------------------------------------------------------------
 */
#define MSM_NPU_IOCTL_MAGIC 'n'

/* get npu info */
#define MSM_NPU_GET_INFO \
	_IOWR(MSM_NPU_IOCTL_MAGIC, 1, struct msm_npu_get_info_ioctl)

/* map buf */
#define MSM_NPU_MAP_BUF \
	_IOWR(MSM_NPU_IOCTL_MAGIC, 2, struct msm_npu_map_buf_ioctl)

/* map buf */
#define MSM_NPU_UNMAP_BUF \
	_IOWR(MSM_NPU_IOCTL_MAGIC, 3, struct msm_npu_unmap_buf_ioctl)

/* load network */
#define MSM_NPU_LOAD_NETWORK \
	_IOWR(MSM_NPU_IOCTL_MAGIC, 4, struct msm_npu_load_network_ioctl)

/* unload network */
#define MSM_NPU_UNLOAD_NETWORK \
	_IOWR(MSM_NPU_IOCTL_MAGIC, 5, struct msm_npu_unload_network_ioctl)

/* exec network */
#define MSM_NPU_EXEC_NETWORK \
	_IOWR(MSM_NPU_IOCTL_MAGIC, 6, struct msm_npu_exec_network_ioctl)

#define MSM_NPU_MAX_INPUT_LAYER_NUM 8
#define MSM_NPU_MAX_OUTPUT_LAYER_NUM 4

/* -------------------------------------------------------------------------
 * Data Structures
 * -------------------------------------------------------------------------
 */
struct msm_npu_patch_info {
	/* chunk id */
	uint32_t chunk_id;
	/* instruction size in bytes */
	uint16_t instruction_size_in_bytes;
	/* variable size in bits */
	uint16_t variable_size_in_bits;
	/* shift value in bits */
	uint16_t shift_value_in_bits;
	/* location offset */
	uint32_t loc_offset;
};

struct msm_npu_layer {
	/* layer id */
	uint32_t layer_id;
	/* patch information*/
	struct msm_npu_patch_info patch_info;
	/* buffer handle */
	int32_t buf_hdl;
	/* buffer size */
	uint32_t buf_size;
	/* physical address */
	uint64_t buf_phys_addr;
};

/* -------------------------------------------------------------------------
 * Data Structures - IOCTLs
 * -------------------------------------------------------------------------
 */
struct msm_npu_map_buf_ioctl {
	/* buffer ion handle */
	int32_t buf_ion_hdl;
	/* buffer size */
	uint32_t size;
	/* iommu mapped physical address */
	uint64_t npu_phys_addr;
};

struct msm_npu_unmap_buf_ioctl {
	/* buffer ion handle */
	int32_t buf_ion_hdl;
	/* iommu mapped physical address */
	uint64_t npu_phys_addr;
};

struct msm_npu_get_info_ioctl {
	/* firmware version */
	uint32_t firmware_version;
	/* reserved */
	uint32_t flags;
};

struct msm_npu_load_network_ioctl {
	/* buffer ion handle */
	int32_t buf_ion_hdl;
	/* physical address */
	uint64_t buf_phys_addr;
	/* buffer size */
	uint32_t buf_size;
	/* first block size */
	uint32_t first_block_size;
	/* reserved */
	uint32_t flags;
	/* network handle */
	uint32_t network_hdl;
	/* priority */
	uint32_t priority;
	/* perf mode */
	uint32_t perf_mode;
};

struct msm_npu_unload_network_ioctl {
	/* network handle */
	uint32_t network_hdl;
};

struct msm_npu_exec_network_ioctl {
	/* network handle */
	uint32_t network_hdl;
	/* input layer number */
	uint32_t input_layer_num;
	/* input layer info */
	struct msm_npu_layer input_layers[MSM_NPU_MAX_INPUT_LAYER_NUM];
	/* output layer number */
	uint32_t output_layer_num;
	/* output layer info */
	struct msm_npu_layer output_layers[MSM_NPU_MAX_OUTPUT_LAYER_NUM];
	/* patching is required */
	uint32_t patching_required;
	/* asynchronous execution */
	uint32_t async;
	/* reserved */
	uint32_t flags;
};

#endif /*_UAPI_MSM_NPU_H_*/
