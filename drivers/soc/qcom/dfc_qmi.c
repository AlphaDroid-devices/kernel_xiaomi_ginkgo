/*
 * Copyright (c) 2018, The Linux Foundation. All rights reserved.
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

#include <linux/rtnetlink.h>
#include <net/pkt_sched.h>
#include <linux/soc/qcom/qmi.h>
#include <soc/qcom/rmnet_qmi.h>

#include <linux/ip.h>
#include "qmi_rmnet_i.h"
#define CREATE_TRACE_POINTS
#include <trace/events/dfc.h>

#define DFC_MAX_BEARERS_V01 16
#define DFC_MAX_QOS_ID_V01 2

struct dfc_qmi_data {
	void *rmnet_port;
	struct workqueue_struct *dfc_wq;
	struct work_struct svc_arrive;
	struct qmi_handle handle;
	struct sockaddr_qrtr ssctl;
	int index;
	int restart_state;
};

struct dfc_svc_ind {
	struct work_struct work;
	struct dfc_qmi_data *data;
	void *dfc_info;
};

struct dfc_burst_ind {
	struct work_struct work;
	struct net_device *dev;
	struct qos_info *qos;
	struct rmnet_bearer_map *bearer;
	struct dfc_qmi_data *data;
};

static void dfc_svc_init(struct work_struct *work);
static void dfc_do_burst_flow_control(struct work_struct *work);

/* **************************************************** */
#define DFC_SERVICE_ID_V01 0x4E
#define DFC_SERVICE_VERS_V01 0x01
#define DFC_TIMEOUT_MS 10000

#define QMI_DFC_BIND_CLIENT_REQ_V01 0x0020
#define QMI_DFC_BIND_CLIENT_RESP_V01 0x0020
#define QMI_DFC_BIND_CLIENT_REQ_V01_MAX_MSG_LEN  11
#define QMI_DFC_BIND_CLIENT_RESP_V01_MAX_MSG_LEN 7

#define QMI_DFC_INDICATION_REGISTER_REQ_V01 0x0001
#define QMI_DFC_INDICATION_REGISTER_RESP_V01 0x0001
#define QMI_DFC_INDICATION_REGISTER_REQ_V01_MAX_MSG_LEN 4
#define QMI_DFC_INDICATION_REGISTER_RESP_V01_MAX_MSG_LEN 7

#define QMI_DFC_FLOW_STATUS_IND_V01 0x0022
#define QMI_DFC_FLOW_STATUS_IND_V01_MAX_MSG_LEN 424

struct dfc_bind_client_req_msg_v01 {
	u8 ep_id_valid;
	struct data_ep_id_type_v01 ep_id;
};

struct dfc_bind_client_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
};

struct dfc_indication_register_req_msg_v01 {
	u8 report_flow_status_valid;
	u8 report_flow_status;
};

struct dfc_indication_register_resp_msg_v01 {
	struct qmi_response_type_v01 resp;
};

enum dfc_ip_type_enum_v01 {
	DFC_IP_TYPE_ENUM_MIN_ENUM_VAL_V01 = -2147483647,
	DFC_IPV4_TYPE_V01 = 0x4,
	DFC_IPV6_TYPE_V01 = 0x6,
	DFC_IP_TYPE_ENUM_MAX_ENUM_VAL_V01 = 2147483647
};

struct dfc_qos_id_type_v01 {
	u32 qos_id;
	enum dfc_ip_type_enum_v01 ip_type;
};

struct dfc_flow_status_info_type_v01 {
	u8 subs_id;
	u8 mux_id;
	u8 bearer_id;
	u32 num_bytes;
	u16 seq_num;
	u8 qos_ids_len;
	struct dfc_qos_id_type_v01 qos_ids[DFC_MAX_QOS_ID_V01];
};

static struct qmi_elem_info dfc_qos_id_type_v01_ei[] = {
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.is_array	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
		.offset		= offsetof(struct dfc_qos_id_type_v01,
					   qos_id),
		.ei_array	= NULL,
	},
	{
		.data_type	= QMI_SIGNED_4_BYTE_ENUM,
		.elem_len	= 1,
		.elem_size	= sizeof(enum dfc_ip_type_enum_v01),
		.is_array	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
		.offset		= offsetof(struct dfc_qos_id_type_v01,
					   ip_type),
		.ei_array	= NULL,
	},
	{
		.data_type	= QMI_EOTI,
		.is_array	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

static struct qmi_elem_info dfc_flow_status_info_type_v01_ei[] = {
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.is_array	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
		.offset		= offsetof(struct
					   dfc_flow_status_info_type_v01,
					   subs_id),
		.ei_array	= NULL,
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.is_array	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
		.offset		= offsetof(struct
					   dfc_flow_status_info_type_v01,
					   mux_id),
		.ei_array	= NULL,
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.is_array	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
		.offset		= offsetof(struct
					   dfc_flow_status_info_type_v01,
					   bearer_id),
		.ei_array	= NULL,
	},
	{
		.data_type	= QMI_UNSIGNED_4_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u32),
		.is_array	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
		.offset		= offsetof(struct
					   dfc_flow_status_info_type_v01,
					   num_bytes),
		.ei_array	= NULL,
	},
	{
		.data_type	= QMI_UNSIGNED_2_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u16),
		.is_array	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
		.offset		= offsetof(struct
					   dfc_flow_status_info_type_v01,
					   seq_num),
		.ei_array	= NULL,
	},
	{
		.data_type	= QMI_DATA_LEN,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.is_array	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
		.offset		= offsetof(struct
					   dfc_flow_status_info_type_v01,
					   qos_ids_len),
		.ei_array	= NULL,
	},
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= DFC_MAX_QOS_ID_V01,
		.elem_size	= sizeof(struct dfc_qos_id_type_v01),
		.is_array	= VAR_LEN_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct
					   dfc_flow_status_info_type_v01,
					   qos_ids),
		.ei_array	= dfc_qos_id_type_v01_ei,
	},
	{
		.data_type	= QMI_EOTI,
		.is_array	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

struct dfc_flow_status_ind_msg_v01 {
	u8 flow_status_valid;
	u8 flow_status_len;
	struct dfc_flow_status_info_type_v01 flow_status[DFC_MAX_BEARERS_V01];
	u8 eod_ack_reqd_valid;
	u8 eod_ack_reqd;
};

static struct qmi_elem_info dfc_bind_client_req_msg_v01_ei[] = {
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.is_array	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct dfc_bind_client_req_msg_v01,
					   ep_id_valid),
		.ei_array	= NULL,
	},
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	= sizeof(struct data_ep_id_type_v01),
		.is_array	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct dfc_bind_client_req_msg_v01,
					   ep_id),
		.ei_array	= data_ep_id_type_v01_ei,
	},
	{
		.data_type	= QMI_EOTI,
		.is_array	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

static struct qmi_elem_info dfc_bind_client_resp_msg_v01_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	= sizeof(struct qmi_response_type_v01),
		.is_array	= NO_ARRAY,
		.tlv_type	= 0x02,
		.offset		= offsetof(struct dfc_bind_client_resp_msg_v01,
					   resp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_EOTI,
		.is_array	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

static struct qmi_elem_info dfc_indication_register_req_msg_v01_ei[] = {
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.is_array	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct
					   dfc_indication_register_req_msg_v01,
					   report_flow_status_valid),
		.ei_array	= NULL,
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.is_array	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct
					   dfc_indication_register_req_msg_v01,
					   report_flow_status),
		.ei_array	= NULL,
	},
	{
		.data_type	= QMI_EOTI,
		.is_array	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

static struct qmi_elem_info dfc_indication_register_resp_msg_v01_ei[] = {
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= 1,
		.elem_size	= sizeof(struct qmi_response_type_v01),
		.is_array	= NO_ARRAY,
		.tlv_type	= 0x02,
		.offset		= offsetof(struct
					   dfc_indication_register_resp_msg_v01,
					   resp),
		.ei_array	= qmi_response_type_v01_ei,
	},
	{
		.data_type	= QMI_EOTI,
		.is_array	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

static struct qmi_elem_info dfc_flow_status_ind_v01_ei[] = {
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.is_array	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct
					   dfc_flow_status_ind_msg_v01,
					   flow_status_valid),
		.ei_array	= NULL,
	},
	{
		.data_type	= QMI_DATA_LEN,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.is_array	= NO_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct
					   dfc_flow_status_ind_msg_v01,
					   flow_status_len),
		.ei_array	= NULL,
	},
	{
		.data_type	= QMI_STRUCT,
		.elem_len	= DFC_MAX_BEARERS_V01,
		.elem_size	= sizeof(struct
					 dfc_flow_status_info_type_v01),
		.is_array	= VAR_LEN_ARRAY,
		.tlv_type	= 0x10,
		.offset		= offsetof(struct
					   dfc_flow_status_ind_msg_v01,
					   flow_status),
		.ei_array	= dfc_flow_status_info_type_v01_ei,
	},
	{
		.data_type	= QMI_OPT_FLAG,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.is_array	= NO_ARRAY,
		.tlv_type	= 0x11,
		.offset		= offsetof(struct
					   dfc_flow_status_ind_msg_v01,
					   eod_ack_reqd_valid),
		.ei_array	= NULL,
	},
	{
		.data_type	= QMI_UNSIGNED_1_BYTE,
		.elem_len	= 1,
		.elem_size	= sizeof(u8),
		.is_array	= NO_ARRAY,
		.tlv_type	= 0x11,
		.offset		= offsetof(struct
					   dfc_flow_status_ind_msg_v01,
					   eod_ack_reqd),
		.ei_array	= NULL,
	},
	{
		.data_type	= QMI_EOTI,
		.is_array	= NO_ARRAY,
		.tlv_type	= QMI_COMMON_TLV_TYPE,
	},
};

static int
dfc_bind_client_req(struct qmi_handle *dfc_handle,
		    struct sockaddr_qrtr *ssctl, struct svc_info *svc)
{
	struct dfc_bind_client_resp_msg_v01 *resp;
	struct dfc_bind_client_req_msg_v01 *req;
	struct qmi_txn txn;
	int ret;

	req = kzalloc(sizeof(*req), GFP_ATOMIC);
	if (!req)
		return -ENOMEM;

	resp = kzalloc(sizeof(*resp), GFP_ATOMIC);
	if (!resp) {
		kfree(req);
		return -ENOMEM;
	}

	ret = qmi_txn_init(dfc_handle, &txn,
			   dfc_bind_client_resp_msg_v01_ei, resp);
	if (ret < 0) {
		pr_err("%s() Failed init for response, err: %d\n",
			__func__, ret);
		goto out;
	}

	req->ep_id_valid = 1;
	req->ep_id.ep_type = svc->ep_type;
	req->ep_id.iface_id = svc->iface_id;
	ret = qmi_send_request(dfc_handle, ssctl, &txn,
			       QMI_DFC_BIND_CLIENT_REQ_V01,
			       QMI_DFC_BIND_CLIENT_REQ_V01_MAX_MSG_LEN,
			       dfc_bind_client_req_msg_v01_ei, req);
	if (ret < 0) {
		qmi_txn_cancel(&txn);
		pr_err("%s() Failed sending request, err: %d\n",
			__func__, ret);
		goto out;
	}

	ret = qmi_txn_wait(&txn, DFC_TIMEOUT_MS);
	if (ret < 0) {
		pr_err("%s() Response waiting failed, err: %d\n",
			__func__, ret);
	} else if (resp->resp.result != QMI_RESULT_SUCCESS_V01) {
		pr_err("%s() Request rejected, result: %d, err: %d\n",
			__func__, resp->resp.result, resp->resp.error);
		ret = -resp->resp.result;
	}

out:
	kfree(resp);
	kfree(req);
	return ret;
}

static int
dfc_indication_register_req(struct qmi_handle *dfc_handle,
			    struct sockaddr_qrtr *ssctl, u8 reg)
{
	struct dfc_indication_register_resp_msg_v01 *resp;
	struct dfc_indication_register_req_msg_v01 *req;
	struct qmi_txn txn;
	int ret;

	req = kzalloc(sizeof(*req), GFP_ATOMIC);
	if (!req)
		return -ENOMEM;

	resp = kzalloc(sizeof(*resp), GFP_ATOMIC);
	if (!resp) {
		kfree(req);
		return -ENOMEM;
	}

	ret = qmi_txn_init(dfc_handle, &txn,
			   dfc_indication_register_resp_msg_v01_ei, resp);
	if (ret < 0) {
		pr_err("%s() Failed init for response, err: %d\n",
			__func__, ret);
		goto out;
	}

	req->report_flow_status_valid = 1;
	req->report_flow_status = reg;
	ret = qmi_send_request(dfc_handle, ssctl, &txn,
			       QMI_DFC_INDICATION_REGISTER_REQ_V01,
			       QMI_DFC_INDICATION_REGISTER_REQ_V01_MAX_MSG_LEN,
			       dfc_indication_register_req_msg_v01_ei, req);
	if (ret < 0) {
		qmi_txn_cancel(&txn);
		pr_err("%s() Failed sending request, err: %d\n",
			__func__, ret);
		goto out;
	}

	ret = qmi_txn_wait(&txn, DFC_TIMEOUT_MS);
	if (ret < 0) {
		pr_err("%s() Response waiting failed, err: %d\n",
			__func__, ret);
	} else if (resp->resp.result != QMI_RESULT_SUCCESS_V01) {
		pr_err("%s() Request rejected, result: %d, err: %d\n",
			__func__, resp->resp.result, resp->resp.error);
		ret = -resp->resp.result;
	}

out:
	kfree(resp);
	kfree(req);
	return ret;
}

static int dfc_init_service(struct dfc_qmi_data *data, struct qmi_info *qmi)
{
	int rc;

	rc = dfc_bind_client_req(&data->handle, &data->ssctl,
				 &qmi->fc_info[data->index].svc);
	if (rc < 0)
		return rc;

	return dfc_indication_register_req(&data->handle, &data->ssctl, 1);
}

static int dfc_bearer_flow_ctl(struct net_device *dev, struct qos_info *qos,
			       u8 bearer_id, u32 grant_size, int enable)
{
	struct list_head *p;
	struct rmnet_flow_map *itm;
	int rc = 0, qlen;

	list_for_each(p, &qos->flow_head) {
		itm = list_entry(p, struct rmnet_flow_map, list);

		if (itm->bearer_id == bearer_id) {
			qlen = tc_qdisc_flow_control(dev, itm->tcm_handle,
						    enable);
			trace_dfc_qmi_tc(itm->bearer_id, itm->flow_id,
					 grant_size, qlen, itm->tcm_handle,
					 enable);
			rc++;
		}
	}
	return rc;
}

static int dfc_all_bearer_flow_ctl(struct net_device *dev,
				struct qos_info *qos, u8 ack_req,
				struct dfc_flow_status_info_type_v01 *fc_info)
{
	struct list_head *p;
	struct rmnet_flow_map *flow_itm;
	struct rmnet_bearer_map *bearer_itm;
	int enable;
	int rc = 0, len;

	list_for_each(p, &qos->bearer_head) {
		bearer_itm = list_entry(p, struct rmnet_bearer_map, list);

		bearer_itm->grant_size = fc_info->num_bytes;
		bearer_itm->seq = fc_info->seq_num;
		bearer_itm->ack_req = ack_req;
	}

	enable = fc_info->num_bytes > 0 ? 1 : 0;

	list_for_each(p, &qos->flow_head) {
		flow_itm = list_entry(p, struct rmnet_flow_map, list);

		len = tc_qdisc_flow_control(dev, flow_itm->tcm_handle, enable);
		trace_dfc_qmi_tc(flow_itm->bearer_id, flow_itm->flow_id,
				 fc_info->num_bytes, len,
				 flow_itm->tcm_handle, enable);
		rc++;
	}
	return rc;
}

static int dfc_update_fc_map(struct net_device *dev, struct qos_info *qos,
			     u8 ack_req,
			     struct dfc_flow_status_info_type_v01 *fc_info)
{
	struct rmnet_bearer_map *itm = NULL;
	int rc = 0;
	int action = -1;

	itm = qmi_rmnet_get_bearer_map(qos, fc_info->bearer_id);
	if (itm) {
		if (itm->grant_size == 0 && fc_info->num_bytes > 0)
			action = 1;
		else if (itm->grant_size > 0 && fc_info->num_bytes == 0)
			action = 0;

		itm->grant_size = fc_info->num_bytes;
		itm->seq = fc_info->seq_num;
		itm->ack_req = ack_req;

		if (action != -1)
			rc = dfc_bearer_flow_ctl(dev, qos, fc_info->bearer_id,
						itm->grant_size, action);
	} else {
		pr_debug("grant %u before flow activate", fc_info->num_bytes);
		qos->default_grant = fc_info->num_bytes;
	}
	return rc;
}

static void dfc_do_burst_flow_control(struct work_struct *work)
{
	struct dfc_svc_ind *svc_ind = (struct dfc_svc_ind *)work;
	struct dfc_flow_status_ind_msg_v01 *ind =
		(struct dfc_flow_status_ind_msg_v01 *)svc_ind->dfc_info;
	struct net_device *dev;
	struct qos_info *qos;
	struct dfc_flow_status_info_type_v01 *flow_status;
	u8 ack_req = ind->eod_ack_reqd_valid ? ind->eod_ack_reqd : 0;
	int i, rc;

	if (!svc_ind->data->rmnet_port) {
		kfree(ind);
		kfree(svc_ind);
		return;
	}

get_lock:
	local_bh_disable();
	/* This will drop some messages but that is
	 * unavoidable for now since the notifier callback is
	 * protected by rtnl_lock() and destroy_workqueue()
	 * will dead lock with this.
	 */
	if (!rtnl_trylock()) {
		if (!svc_ind->data->restart_state) {
			local_bh_enable();
			msleep(20);
			goto get_lock;
		} else {
			kfree(ind);
			kfree(svc_ind);
			local_bh_enable();
			return;
		}
	}

	if (unlikely(svc_ind->data->restart_state))
		goto clean_out;

	for (i = 0; i < ind->flow_status_len; i++) {
		flow_status = &ind->flow_status[i];
		trace_dfc_flow_ind(svc_ind->data->index,
				   i, flow_status->mux_id,
				   flow_status->bearer_id,
				   flow_status->num_bytes,
				   flow_status->seq_num,
				   ack_req);
		dev = rmnet_get_rmnet_dev(svc_ind->data->rmnet_port,
					  flow_status->mux_id);
		if (!dev)
			goto clean_out;

		qos = (struct qos_info *)rmnet_get_qos_pt(dev);
		if (!qos)
			continue;

		if (unlikely(flow_status->bearer_id == 0xFF))
			rc = dfc_all_bearer_flow_ctl(
				dev, qos, ack_req, flow_status);
		else
			rc = dfc_update_fc_map(dev, qos, ack_req, flow_status);
	}

clean_out:
	kfree(ind);
	kfree(svc_ind);
	rtnl_unlock();
	local_bh_enable();
}

static void dfc_bearer_limit_work(struct work_struct *work)
{
	struct dfc_burst_ind *dfc_ind = (struct dfc_burst_ind *)work;
	struct rmnet_flow_map *itm;
	struct list_head *p;
	int qlen, fc;

	local_bh_disable();

	/* enable transmit on device so that the other
	 * flows which transmit proceed normally.
	 * do it here under bh disabled so that the TX softirq
	 * may not run here
	 */
	netif_start_queue(dfc_ind->dev);

	while (!rtnl_trylock()) {
		if (!dfc_ind->data->restart_state) {
			cond_resched_softirq();
		} else {
			kfree(dfc_ind);
			local_bh_enable();
			return;
		}
	}

	fc = dfc_ind->bearer->grant_size ? 1 : 0;
	/* if grant size is non zero here, we must have already
	 * got an updated grant. do nothing in that case
	 */
	if (fc)
		goto done;

	list_for_each(p, &dfc_ind->qos->flow_head) {
		itm = list_entry(p, struct rmnet_flow_map, list);

		if (itm->bearer_id == dfc_ind->bearer->bearer_id) {
			qlen = tc_qdisc_flow_control(dfc_ind->dev,
						     itm->tcm_handle, fc);
			trace_dfc_qmi_tc_limit(itm->bearer_id, itm->flow_id,
					       dfc_ind->bearer->grant_size,
					       qlen, itm->tcm_handle, fc);
		}
	}

done:
	kfree(dfc_ind);
	rtnl_unlock();
	local_bh_enable();
}

static void dfc_clnt_ind_cb(struct qmi_handle *qmi, struct sockaddr_qrtr *sq,
			    struct qmi_txn *txn, const void *data)
{
	struct dfc_qmi_data *dfc = container_of(qmi, struct dfc_qmi_data,
						handle);
	struct dfc_flow_status_ind_msg_v01 *ind_msg;
	struct dfc_svc_ind *svc_ind;

	if (!dfc->rmnet_port)
		return;

	if (qmi != &dfc->handle)
		return;

	ind_msg = (struct dfc_flow_status_ind_msg_v01 *)data;
	if (ind_msg->flow_status_valid) {
		if (ind_msg->flow_status_len > DFC_MAX_BEARERS_V01) {
			pr_err("%s() Invalid fc info len: %d\n",
			       __func__, ind_msg->flow_status_len);
			return;
		}

		svc_ind = kzalloc(sizeof(struct dfc_svc_ind), GFP_ATOMIC);
		if (!svc_ind)
			return;

		INIT_WORK((struct work_struct *)svc_ind,
			  dfc_do_burst_flow_control);
		svc_ind->dfc_info = kzalloc(sizeof(*ind_msg), GFP_ATOMIC);
		if (!svc_ind->dfc_info) {
			kfree(svc_ind);
			return;
		}

		memcpy(svc_ind->dfc_info, ind_msg, sizeof(*ind_msg));
		svc_ind->data = dfc;
		queue_work(dfc->dfc_wq, (struct work_struct *)svc_ind);
	}
}

static void dfc_svc_init(struct work_struct *work)
{
	int rc = 0;
	struct dfc_qmi_data *data = container_of(work, struct dfc_qmi_data,
						 svc_arrive);
	struct qmi_info *qmi;

	qmi = (struct qmi_info *)rmnet_get_qmi_pt(data->rmnet_port);
	if (!qmi)
		goto clean_out;

	rc = dfc_init_service(data, qmi);
	if (rc < 0)
		goto clean_out;

	qmi->fc_info[data->index].dfc_client = (void *)data;
	trace_dfc_client_state_up(data->index,
				  qmi->fc_info[data->index].svc.instance,
				  qmi->fc_info[data->index].svc.ep_type,
				  qmi->fc_info[data->index].svc.iface_id);
	return;

clean_out:
	qmi_handle_release(&data->handle);
	destroy_workqueue(data->dfc_wq);
	kfree(data);
}

static int dfc_svc_arrive(struct qmi_handle *qmi, struct qmi_service *svc)
{
	struct dfc_qmi_data *data = container_of(qmi, struct dfc_qmi_data,
						 handle);

	data->ssctl.sq_family = AF_QIPCRTR;
	data->ssctl.sq_node = svc->node;
	data->ssctl.sq_port = svc->port;

	queue_work(data->dfc_wq, &data->svc_arrive);

	return 0;
}

static void dfc_svc_exit(struct qmi_handle *qmi, struct qmi_service *svc)
{
	struct dfc_qmi_data *data = container_of(qmi, struct dfc_qmi_data,
						 handle);

	if (!data)
		pr_debug("%s() data is null\n", __func__);
}

static struct qmi_ops server_ops = {
	.new_server = dfc_svc_arrive,
	.del_server = dfc_svc_exit,
};

static struct qmi_msg_handler qmi_indication_handler[] = {
	{
		.type = QMI_INDICATION,
		.msg_id = QMI_DFC_FLOW_STATUS_IND_V01,
		.ei = dfc_flow_status_ind_v01_ei,
		.decoded_size = QMI_DFC_FLOW_STATUS_IND_V01_MAX_MSG_LEN,
		.fn = dfc_clnt_ind_cb,
	},
	{},
};

int dfc_qmi_client_init(void *port, int index, struct qmi_info *qmi)
{
	struct dfc_qmi_data *data;
	int rc = -ENOMEM;

	data = kzalloc(sizeof(struct dfc_qmi_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->rmnet_port = port;
	data->index = index;
	data->restart_state = 0;

	data->dfc_wq = alloc_workqueue("dfc_wq", WQ_HIGHPRI, 1);
	if (!data->dfc_wq) {
		pr_err("%s Could not create workqueue\n", __func__);
		goto err0;
	}

	INIT_WORK(&data->svc_arrive, dfc_svc_init);
	rc = qmi_handle_init(&data->handle,
			     QMI_DFC_FLOW_STATUS_IND_V01_MAX_MSG_LEN,
			     &server_ops, qmi_indication_handler);
	if (rc < 0) {
		pr_err("%s: failed qmi_handle_init - rc[%d]\n", __func__, rc);
		goto err1;
	}

	rc = qmi_add_lookup(&data->handle, DFC_SERVICE_ID_V01,
			    DFC_SERVICE_VERS_V01,
			    qmi->fc_info[index].svc.instance);
	if (rc < 0) {
		pr_err("%s: failed qmi_add_lookup - rc[%d]\n", __func__, rc);
		goto err2;
	}

	return 0;

err2:
	qmi_handle_release(&data->handle);
err1:
	destroy_workqueue(data->dfc_wq);
err0:
	kfree(data);
	return rc;
}

void dfc_qmi_client_exit(void *dfc_data)
{
	struct dfc_qmi_data *data = (struct dfc_qmi_data *)dfc_data;

	if (!data) {
		pr_err("%s() data is null\n", __func__);
		return;
	}

	data->restart_state = 1;
	trace_dfc_client_state_down(data->index, 0);
	qmi_handle_release(&data->handle);

	drain_workqueue(data->dfc_wq);
	destroy_workqueue(data->dfc_wq);
	kfree(data);
}

void dfc_qmi_burst_check(struct net_device *dev, struct qos_info *qos,
			 struct sk_buff *skb, struct qmi_info *qmi)
{
	struct rmnet_bearer_map *bearer;
	struct dfc_burst_ind *dfc_ind;
	struct rmnet_flow_map *itm;
	struct dfc_qmi_data *data;
	int ip_type;

	ip_type = (ip_hdr(skb)->version == IP_VER_6) ? AF_INET6 : AF_INET;

	itm = qmi_rmnet_get_flow_map(qos, skb->mark, ip_type);
	if (!itm)
		return;

	bearer = qmi_rmnet_get_bearer_map(qos, itm->bearer_id);
	if (unlikely(!bearer))
		return;

	trace_dfc_flow_check(bearer->bearer_id, skb->len, bearer->grant_size);

	if (!bearer->grant_size)
		return;

	if (skb->len < bearer->grant_size) {
		bearer->grant_size -= skb->len;
		return;
	}

	data = (struct dfc_qmi_data *)qmi_rmnet_has_dfc_client(qmi);
	if (!data)
		return;

	dfc_ind = kzalloc(sizeof(*dfc_ind), GFP_ATOMIC);
	if (!dfc_ind)
		return;

	INIT_WORK((struct work_struct *)dfc_ind, dfc_bearer_limit_work);

	dfc_ind->dev = dev;
	dfc_ind->qos = qos;
	dfc_ind->bearer = bearer;
	dfc_ind->data = data;

	bearer->grant_size = 0;

	/* stop the flow in hope that the worker thread is
	 * immediately scheduled beyond this point of time
	 */
	netif_stop_queue(dev);
	queue_work(data->dfc_wq, (struct work_struct *)dfc_ind);
}
