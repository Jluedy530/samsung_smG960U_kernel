/* Copyright (c) 2017, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef _CAM_ISP_CONTEXT_H_
#define _CAM_ISP_CONTEXT_H_


#include <linux/spinlock.h>
#include <uapi/media/cam_isp.h>

#include "cam_context.h"
#include "cam_isp_hw_mgr_intf.h"

/*
 * Maximum hw resource - This number is based on the maximum
 * output port resource. The current maximum resource number
 * is 20.
 */
#define CAM_ISP_CTX_RES_MAX                     20

/*
 * Maxiimum configuration entry size  - This is based on the
 * worst case DUAL IFE use case plus some margin.
 */
#define CAM_ISP_CTX_CFG_MAX                     22

/* forward declaration */
struct cam_isp_context;

/* cam isp context irq handling function type */
typedef int (*cam_isp_hw_event_cb_func)(struct cam_isp_context *ctx_isp,
	void *evt_data);

/**
 * enum cam_isp_ctx_activated_substate - sub states for activated
 *
 */
enum cam_isp_ctx_activated_substate {
	CAM_ISP_CTX_ACTIVATED_SOF,
	CAM_ISP_CTX_ACTIVATED_APPLIED,
	CAM_ISP_CTX_ACTIVATED_EPOCH,
	CAM_ISP_CTX_ACTIVATED_BUBBLE,
	CAM_ISP_CTX_ACTIVATED_BUBBLE_APPLIED,
	CAM_ISP_CTX_ACTIVATED_HW_ERROR,
	CAM_ISP_CTX_ACTIVATED_HALT,
	CAM_ISP_CTX_ACTIVATED_FLUSH,
	CAM_ISP_CTX_ACTIVATED_MAX,
};


/**
 * struct cam_isp_ctx_irq_ops - Function table for handling IRQ callbacks
 *
 * @irq_ops:               Array of handle function pointers.
 *
 */
struct cam_isp_ctx_irq_ops {
	cam_isp_hw_event_cb_func         irq_ops[CAM_ISP_HW_EVENT_MAX];
};

/**
 * struct cam_isp_ctx_req - ISP context request object
 *
 * @base:                  Common request object ponter
 * @cfg:                   ISP hardware configuration array
 * @num_cfg:               Number of ISP hardware configuration entries
 * @fence_map_out:         Output fence mapping array
 * @num_fence_map_out:     Number of the output fence map
 * @fence_map_in:          Input fence mapping array
 * @num_fence_map_in:      Number of input fence map
 * @num_acked:             Count to track acked entried for output.
 *                         If count equals the number of fence out, it means
 *                         the request has been completed.
 * @bubble_report:         Flag to track if bubble report is active on
 *                         current request
 * @packet_opcode_type:    Request packet opcode type,
 *                         ie INIT packet or update packet
 *
 */
struct cam_isp_ctx_req {
	struct cam_ctx_request           *base;

	struct cam_hw_update_entry        cfg[CAM_ISP_CTX_CFG_MAX];
	uint32_t                          num_cfg;
	struct cam_hw_fence_map_entry     fence_map_out[CAM_ISP_CTX_RES_MAX];
	uint32_t                          num_fence_map_out;
	struct cam_hw_fence_map_entry     fence_map_in[CAM_ISP_CTX_RES_MAX];
	uint32_t                          num_fence_map_in;
	uint32_t                          num_acked;
	int32_t                           bubble_report;
	uint32_t                          packet_opcode_type;
	struct cam_isp_bw_config_internal bw_config[CAM_IFE_HW_NUM_MAX];
	bool                              bw_config_valid[CAM_IFE_HW_NUM_MAX];
};

/**
 * struct cam_isp_context  - ISP context object
 *
 * @base:                  Common context object pointer
 * @frame_id:              Frame id tracking for the isp context
 * @substate_actiavted:    Current substate for the activated state.
 * @substate_machine:      ISP substate machine for external interface
 * @substate_machine_irq:  ISP substate machine for irq handling
 * @req_base:              Common request object storage
 * @req_isp:               ISP private request object storage
 * @hw_ctx:                HW object returned by the acquire device command
 * @sof_timestamp_val:     Captured time stamp value at sof hw event
 * @active_req_cnt:        Counter for the active request
 * @reported_req_id:       Last reported request id
 * @subscribe_event:       The irq event mask that CRM subscribes to, IFE will
 *                         invoke CRM cb at those event.
 * @last_applied_req_id:   Last applied request id
 *
 */
struct cam_isp_context {
	struct cam_context              *base;

	int64_t                          frame_id;
	uint32_t                         substate_activated;
	struct cam_ctx_ops              *substate_machine;
	struct cam_isp_ctx_irq_ops      *substate_machine_irq;

	struct cam_ctx_request           req_base[CAM_CTX_REQ_MAX];
	struct cam_isp_ctx_req           req_isp[CAM_CTX_REQ_MAX];

	void                            *hw_ctx;
	uint64_t                         sof_timestamp_val;
	int32_t                          active_req_cnt;
	int64_t                          reported_req_id;
	uint32_t                         subscribe_event;
	int64_t                          last_applied_req_id;
	uint32_t                         frame_skip_count;
};

/**
 * cam_isp_context_init()
 *
 * @brief:              Initialization function for the ISP context
 *
 * @ctx:                ISP context obj to be initialized
 * @bridge_ops:         Bridge call back funciton
 * @hw_intf:            ISP hw manager interface
 * @ctx_id:             ID for this context
 *
 */
int cam_isp_context_init(struct cam_isp_context *ctx,
	struct cam_context *ctx_base,
	struct cam_req_mgr_kmd_ops *bridge_ops,
	struct cam_hw_mgr_intf *hw_intf,
	uint32_t ctx_id);

/**
 * cam_isp_context_deinit()
 *
 * @brief:               Deinitialize function for the ISP context
 *
 * @ctx:                 ISP context obj to be deinitialized
 *
 */
int cam_isp_context_deinit(struct cam_isp_context *ctx);


#endif  /* __CAM_ISP_CONTEXT_H__ */
