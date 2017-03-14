/*
 * Copyright (c) 2012-2017 The Linux Foundation. All rights reserved.
 *
 * Previously licensed under the ISC license by Qualcomm Atheros, Inc.
 *
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * This file was originally distributed by Qualcomm Atheros, Inc.
 * under proprietary terms before Copyright ownership was assigned
 * to the Linux Foundation.
 */

/**
 * DOC: cds_api.c
 *
 * Connectivity driver services APIs
 */

#include "cds_sched.h"
#include <cds_api.h>
#include "sir_types.h"
#include "sir_api.h"
#include "sir_mac_prot_def.h"
#include "sme_api.h"
#include "mac_init_api.h"
#include "wlan_qct_sys.h"
#include "i_cds_packet.h"
#include "cds_reg_service.h"
#include "wma_types.h"
#include "wlan_hdd_main.h"
#include <linux/vmalloc.h>

#include "pld_common.h"
#include "sap_api.h"
#include "qdf_trace.h"
#include "bmi.h"
#include "ol_fw.h"
#include "ol_if_athvar.h"
#include "hif.h"
#include "cds_concurrency.h"
#include "cds_utils.h"
#include "wlan_logging_sock_svc.h"
#include "wma.h"
#include "pktlog_ac.h"
#include "wlan_hdd_ipa.h"

#include <cdp_txrx_cmn_reg.h>
#include <cdp_txrx_cfg.h>
#include <cdp_txrx_misc.h>
#include <dispatcher_init_deinit.h>
#include <cdp_txrx_handle.h>

/* Preprocessor Definitions and Constants */

/* Data definitions */
static cds_context_type g_cds_context;
static p_cds_contextType gp_cds_context;
static struct __qdf_device g_qdf_ctx;

static uint8_t cds_multicast_logging;

static struct ol_if_ops  dp_ol_if_ops = {
	.peer_set_default_routing = wma_peer_set_default_routing,
	.peer_rx_reorder_queue_setup = wma_peer_rx_reorder_queue_setup,
	.peer_rx_reorder_queue_remove = wma_peer_rx_reorder_queue_remove,
	.is_hw_dbs_2x2_capable = wma_is_hw_dbs_2x2_capable
    /* TODO: Add any other control path calls required to OL_IF/WMA layer */
};

void cds_sys_probe_thread_cback(void *pUserData);

/**
 * cds_init() - Initialize CDS
 *
 * This function allocates the resource required for CDS, but does not
 * initialize all the members. This overall initialization will happen at
 * cds_open().
 *
 * Return: Global context on success and NULL on failure.
 */
v_CONTEXT_t cds_init(void)
{
	qdf_debugfs_init();
	qdf_lock_stats_init();
	qdf_mem_init();
	qdf_mc_timer_manager_init();

	gp_cds_context = &g_cds_context;

	gp_cds_context->qdf_ctx = &g_qdf_ctx;
	qdf_mem_zero(&g_qdf_ctx, sizeof(g_qdf_ctx));

	qdf_trace_spin_lock_init();

#if defined(TRACE_RECORD)
	qdf_trace_init();
#endif
	qdf_register_debugcb_init();

	cds_ssr_protect_init();

	return gp_cds_context;
}

/**
 * cds_deinit() - Deinitialize CDS
 *
 * This function frees the CDS resources
 */
void cds_deinit(void)
{
	if (gp_cds_context == NULL)
		return;

	qdf_mc_timer_manager_exit();
	qdf_mem_exit();
	qdf_lock_stats_deinit();
	qdf_debugfs_exit();

	gp_cds_context->qdf_ctx = NULL;
	gp_cds_context = NULL;

	qdf_mem_zero(&g_cds_context, sizeof(g_cds_context));
	return;
}

#ifdef FEATURE_WLAN_DIAG_SUPPORT
/**
 * cds_tdls_tx_rx_mgmt_event()- send tdls mgmt rx tx event
 * @event_id: event id
 * @tx_rx: tx or rx
 * @type: type of frame
 * @action_sub_type: action frame type
 * @peer_mac: peer mac
 *
 * This Function sends tdls mgmt rx tx diag event
 *
 * Return: void.
 */
void cds_tdls_tx_rx_mgmt_event(uint8_t event_id, uint8_t tx_rx,
		uint8_t type, uint8_t action_sub_type, uint8_t *peer_mac)
{
	WLAN_HOST_DIAG_EVENT_DEF(tdls_tx_rx_mgmt,
		struct host_event_tdls_tx_rx_mgmt);

	tdls_tx_rx_mgmt.event_id = event_id;
	tdls_tx_rx_mgmt.tx_rx = tx_rx;
	tdls_tx_rx_mgmt.type = type;
	tdls_tx_rx_mgmt.action_sub_type = action_sub_type;
	qdf_mem_copy(tdls_tx_rx_mgmt.peer_mac,
			peer_mac, CDS_MAC_ADDRESS_LEN);
	WLAN_HOST_DIAG_EVENT_REPORT(&tdls_tx_rx_mgmt,
				EVENT_WLAN_TDLS_TX_RX_MGMT);
}
#endif

/**
 * cds_cfg_update_ac_specs_params() - update ac_specs params
 * @olcfg: cfg handle
 * @mac_params: mac params
 *
 * Return: none
 */
static void
cds_cfg_update_ac_specs_params(struct txrx_pdev_cfg_param_t *olcfg,
		struct cds_config_info *cds_cfg)
{
	int i;

	if (NULL == olcfg)
		return;

	if (NULL == cds_cfg)
		return;

	for (i = 0; i < OL_TX_NUM_WMM_AC; i++) {
		olcfg->ac_specs[i].wrr_skip_weight =
			cds_cfg->ac_specs[i].wrr_skip_weight;
		olcfg->ac_specs[i].credit_threshold =
			cds_cfg->ac_specs[i].credit_threshold;
		olcfg->ac_specs[i].send_limit =
			cds_cfg->ac_specs[i].send_limit;
		olcfg->ac_specs[i].credit_reserve =
			cds_cfg->ac_specs[i].credit_reserve;
		olcfg->ac_specs[i].discard_weight =
			cds_cfg->ac_specs[i].discard_weight;
	}
}

/**
 * cds_cdp_cfg_attach() - attach data path config module
 * @cds_cfg: generic platform level config instance
 *
 * Return: none
 */
static void cds_cdp_cfg_attach(struct cds_config_info *cds_cfg)
{
	struct txrx_pdev_cfg_param_t cdp_cfg = {0};
	void *soc = cds_get_context(QDF_MODULE_ID_SOC);

	cdp_cfg.is_full_reorder_offload = cds_cfg->reorder_offload;
	cdp_cfg.is_uc_offload_enabled = cds_cfg->uc_offload_enabled;
	cdp_cfg.uc_tx_buffer_count = cds_cfg->uc_txbuf_count;
	cdp_cfg.uc_tx_buffer_size = cds_cfg->uc_txbuf_size;
	cdp_cfg.uc_rx_indication_ring_count = cds_cfg->uc_rxind_ringcount;
	cdp_cfg.uc_tx_partition_base = cds_cfg->uc_tx_partition_base;
	cdp_cfg.enable_rxthread = cds_cfg->enable_rxthread;
	cdp_cfg.ip_tcp_udp_checksum_offload =
			cds_cfg->ip_tcp_udp_checksum_offload;
	cdp_cfg.ce_classify_enabled = cds_cfg->ce_classify_enabled;

	cds_cfg_update_ac_specs_params(&cdp_cfg, cds_cfg);
	gp_cds_context->cfg_ctx = cdp_cfg_attach(soc, gp_cds_context->qdf_ctx,
					(void *)(&cdp_cfg));
	if (!gp_cds_context->cfg_ctx) {
		WMA_LOGP("%s: failed to init cfg handle", __func__);
		return;
	}

	/* Configure Receive flow steering */
	cdp_cfg_set_flow_steering(soc, gp_cds_context->cfg_ctx,
				 cds_cfg->flow_steering_enabled);

	cdp_cfg_set_flow_control_parameters(soc, gp_cds_context->cfg_ctx,
			(void *)&cdp_cfg);

	/* adjust the cfg_ctx default value based on setting */
	cdp_cfg_set_rx_fwd_disabled(soc, gp_cds_context->cfg_ctx,
		(uint8_t) cds_cfg->ap_disable_intrabss_fwd);

	/*
	 * adjust the packet log enable default value
	 * based on CFG INI setting
	 */
	cdp_cfg_set_packet_log_enabled(soc, gp_cds_context->cfg_ctx,
		(uint8_t)cds_is_packet_log_enabled());
}
static QDF_STATUS cds_register_all_modules(void)
{
	QDF_STATUS status;

	scheduler_register_wma_legacy_handler(&wma_mc_process_handler);
	scheduler_register_sys_legacy_handler(&sys_mc_process_handler);

	/* Register message queues in given order such that queue priority is
	 * intact:
	 * 1) QDF_MODULE_ID_SYS: Timer queue(legacy SYS queue)
	 * 2) QDF_MODULE_ID_TARGET_IF: Target interface queue
	 * 3) QDF_MODULE_ID_PE: Legacy PE message queue
	 * 4) QDF_MODULE_ID_SME: Legacy SME message queue
	 * 5) QDF_MODULE_ID_OS_IF: OS IF message queue for new components
	 */
	status = scheduler_register_module(QDF_MODULE_ID_SYS,
					&scheduler_timer_q_mq_handler);
	status = scheduler_register_module(QDF_MODULE_ID_TARGET_IF,
					&scheduler_target_if_mq_handler);
	status = scheduler_register_module(QDF_MODULE_ID_PE,
					&pe_mc_process_handler);
	status = scheduler_register_module(QDF_MODULE_ID_SME,
					&sme_mc_process_handler);
	status = scheduler_register_module(QDF_MODULE_ID_OS_IF,
					&scheduler_os_if_mq_handler);
	return status;
}

static QDF_STATUS cds_deregister_all_modules(void)
{
	QDF_STATUS status;

	scheduler_deregister_wma_legacy_handler();
	scheduler_deregister_sys_legacy_handler();
	status = scheduler_deregister_module(QDF_MODULE_ID_SYS);
	status = scheduler_deregister_module(QDF_MODULE_ID_TARGET_IF);
	status = scheduler_deregister_module(QDF_MODULE_ID_PE);
	status = scheduler_deregister_module(QDF_MODULE_ID_SME);
	status = scheduler_deregister_module(QDF_MODULE_ID_OS_IF);

	return status;
}

/**
 * cds_set_ac_specs_params() - set ac_specs params in cds_config_info
 * @cds_cfg: Pointer to cds_config_info
 * @hdd_ctx: Pointer to hdd context
 *
 * Return: none
 */
static void
cds_set_ac_specs_params(struct cds_config_info *cds_cfg)
{
	int i;
	cds_context_type *cds_ctx;

	if (NULL == cds_cfg)
		return;

	cds_ctx = cds_get_context(QDF_MODULE_ID_QDF);

	if (!cds_ctx) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			"Invalid CDS Context");
		return;
	}

	for (i = 0; i < OL_TX_NUM_WMM_AC; i++) {
		cds_cfg->ac_specs[i] = cds_ctx->ac_specs[i];
	}
}

/**
 * cds_open() - open the CDS Module
 *
 * cds_open() function opens the CDS Scheduler
 * Upon successful initialization:
 * - All CDS submodules should have been initialized
 *
 * - The CDS scheduler should have opened
 *
 * - All the WLAN SW components should have been opened. This includes
 * SYS, MAC, SME, WMA and TL.
 *
 * Return: QDF status
 */
QDF_STATUS cds_open(struct wlan_objmgr_psoc *psoc)
{
	QDF_STATUS qdf_status = QDF_STATUS_SUCCESS;
	tSirRetStatus sirStatus = eSIR_SUCCESS;
	struct cds_config_info *cds_cfg;
	qdf_device_t qdf_ctx;
	HTC_INIT_INFO htcInfo;
	struct ol_context *ol_ctx;
	struct hif_opaque_softc *scn;
	void *HTCHandle;
	hdd_context_t *pHddCtx;
	cds_context_type *cds_ctx;

	QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_INFO_HIGH,
		  "%s: Opening CDS", __func__);

	cds_ctx = cds_get_context(QDF_MODULE_ID_QDF);
	if (!cds_ctx) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_FATAL,
			  "%s: Trying to open CDS without a PreOpen", __func__);
		QDF_ASSERT(0);
		return QDF_STATUS_E_FAILURE;
	}

	/* Initialize the timer module */
	qdf_timer_module_init();

	/* Initialize bug reporting structure */
	cds_init_log_completion();

	/* Initialize the probe event */
	if (qdf_event_create(&gp_cds_context->ProbeEvent) != QDF_STATUS_SUCCESS) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_FATAL,
			  "%s: Unable to init probeEvent", __func__);
		QDF_ASSERT(0);
		return QDF_STATUS_E_FAILURE;
	}
	if (qdf_event_create(&(gp_cds_context->wmaCompleteEvent)) !=
	    QDF_STATUS_SUCCESS) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_FATAL,
			  "%s: Unable to init wmaCompleteEvent", __func__);
		QDF_ASSERT(0);
		goto err_probe_event;
	}

	pHddCtx = (hdd_context_t *) (gp_cds_context->pHDDContext);
	if ((NULL == pHddCtx) || (NULL == pHddCtx->config)) {
		/* Critical Error ...  Cannot proceed further */
		cds_err("Hdd Context is Null");
		QDF_ASSERT(0);
		goto err_wma_complete_event;
	}

	if (!QDF_IS_STATUS_SUCCESS(qdf_mutex_create(
				&cds_ctx->qdf_conc_list_lock))) {
		cds_err("Failed to init qdf_conc_list_lock");
		QDF_ASSERT(0);
		goto err_wma_complete_event;
	}

	/* Now Open the CDS Scheduler */

	if (pHddCtx->driver_status == DRIVER_MODULES_UNINITIALIZED ||
	    cds_is_driver_recovering()) {
		qdf_status = cds_sched_open(gp_cds_context,
					    &gp_cds_context->qdf_sched,
					    sizeof(cds_sched_context));

		if (!QDF_IS_STATUS_SUCCESS(qdf_status)) {
			/* Critical Error ...  Cannot proceed further */
			QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_FATAL,
				  "%s: Failed to open CDS Scheduler", __func__);
			QDF_ASSERT(0);
			goto err_concurrency_lock;
		}
	}

	scn = cds_get_context(QDF_MODULE_ID_HIF);
	if (!scn) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_FATAL,
			  "%s: scn is null!", __func__);
		goto err_sched_close;
	}

	cds_cfg = cds_get_ini_config();
	if (!cds_cfg) {
		cds_err("Cds config is NULL");
		QDF_ASSERT(0);
		goto err_sched_close;
	}
	hdd_enable_fastpath(pHddCtx->config, scn);
	hdd_wlan_update_target_info(pHddCtx, scn);

	ol_ctx = cds_get_context(QDF_MODULE_ID_BMI);
	/* Initialize BMI and Download firmware */
	qdf_status = bmi_download_firmware(ol_ctx);
	if (qdf_status != QDF_STATUS_SUCCESS) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_FATAL,
			  "BMI FIALED status:%d", qdf_status);
		goto err_bmi_close;
	}
	htcInfo.pContext = ol_ctx;
	htcInfo.TargetFailure = ol_target_failure;
	htcInfo.TargetSendSuspendComplete =
		pmo_ucfg_psoc_target_suspend_acknowledge;
	htcInfo.target_initial_wakeup_cb = pmo_ucfg_psoc_handle_initial_wake_up;
	htcInfo.target_psoc = (void *)psoc;
	qdf_ctx = cds_get_context(QDF_MODULE_ID_QDF_DEVICE);

	/* Create HTC */
	gp_cds_context->htc_ctx =
		htc_create(scn, &htcInfo, qdf_ctx, cds_get_conparam());
	if (!gp_cds_context->htc_ctx) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_FATAL,
			  "%s: Failed to Create HTC", __func__);
		goto err_bmi_close;
	}
	pmo_ucfg_psoc_update_htc_handle(psoc, (void *)gp_cds_context->htc_ctx);

	if (bmi_done(ol_ctx)) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_FATAL,
			  "%s: Failed to complete BMI phase", __func__);
		goto err_htc_close;
	}

	/*Open the WMA module */
	qdf_status = wma_open(psoc, gp_cds_context,
			      hdd_update_tgt_cfg,
			      hdd_dfs_indicate_radar, cds_cfg);

	if (!QDF_IS_STATUS_SUCCESS(qdf_status)) {
		/* Critical Error ...  Cannot proceed further */
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_FATAL,
			  "%s: Failed to open WMA module", __func__);
		QDF_ASSERT(0);
		goto err_htc_close;
	}

	/* Number of peers limit differs in each chip version. If peer max
	 * limit configured in ini exceeds more than supported, WMA adjusts
	 * and keeps correct limit in cds_cfg.max_station. So, make sure
	 * config entry pHddCtx->config->maxNumberOfPeers has adjusted value
	 */
	/* In FTM mode cds_cfg->max_stations will be zero. On updating same
	 * into hdd context config entry, leads to pe_open() to fail, if
	 * con_mode change happens from FTM mode to any other mode.
	 */
	if (DRIVER_TYPE_PRODUCTION == cds_cfg->driver_type)
		pHddCtx->config->maxNumberOfPeers = cds_cfg->max_station;

	HTCHandle = cds_get_context(QDF_MODULE_ID_HTC);
	if (!HTCHandle) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_FATAL,
			  "%s: HTCHandle is null!", __func__);
		goto err_wma_close;
	}
	if (htc_wait_target(HTCHandle)) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_FATAL,
			  "%s: Failed to complete BMI phase", __func__);
		goto err_wma_close;
	}
	bmi_target_ready(scn, gp_cds_context->cfg_ctx);

	QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_DEBUG,
		"%s: target_type %d 8074:%d 6290:%d",
		__func__, pHddCtx->target_type,
		TARGET_TYPE_QCA8074, TARGET_TYPE_QCA6290);

	if (TARGET_TYPE_QCA6290 == pHddCtx->target_type)
		gp_cds_context->dp_soc = cdp_soc_attach(LITHIUM_DP,
			gp_cds_context->pHIFContext, scn,
			gp_cds_context->htc_ctx, gp_cds_context->qdf_ctx,
			&dp_ol_if_ops);
	else
		gp_cds_context->dp_soc = cdp_soc_attach(MOB_DRV_LEGACY_DP,
			gp_cds_context->pHIFContext, scn,
			gp_cds_context->htc_ctx, gp_cds_context->qdf_ctx,
			&dp_ol_if_ops);

	cds_set_ac_specs_params(cds_cfg);

	cds_cdp_cfg_attach(cds_cfg);

	/* Now proceed to open the MAC */
	sirStatus =
		mac_open(psoc, &(gp_cds_context->pMACContext),
			gp_cds_context->pHDDContext, cds_cfg);

	if (eSIR_SUCCESS != sirStatus) {
		/* Critical Error ...  Cannot proceed further */
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_FATAL,
			  "%s: Failed to open MAC", __func__);
		QDF_ASSERT(0);
		goto err_wma_close;
	}

	/* Now proceed to open the SME */
	qdf_status = sme_open(gp_cds_context->pMACContext);
	if (!QDF_IS_STATUS_SUCCESS(qdf_status)) {
		/* Critical Error ...  Cannot proceed further */
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_FATAL,
			  "%s: Failed to open SME", __func__);
		QDF_ASSERT(0);
		goto err_mac_close;
	}
	cds_set_context(QDF_MODULE_ID_TXRX,
		cdp_pdev_attach(cds_get_context(QDF_MODULE_ID_SOC),
			gp_cds_context->cfg_ctx,
			gp_cds_context->htc_ctx,
			gp_cds_context->qdf_ctx, 0));
	if (!gp_cds_context->pdev_txrx_ctx) {
		/* Critical Error ...  Cannot proceed further */
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_FATAL,
			  "%s: Failed to open TXRX", __func__);
		QDF_ASSERT(0);
		goto err_sme_close;
	}

	gp_cds_context->cdp_update_mac_id = cdp_update_mac_id;
	QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_INFO_HIGH,
		  "%s: CDS successfully Opened", __func__);
	cds_register_all_modules();

	return dispatcher_psoc_open(psoc);

err_sme_close:
	sme_close(gp_cds_context->pMACContext);

err_mac_close:
	mac_close(gp_cds_context->pMACContext);

err_wma_close:
	cds_shutdown_notifier_purge();
	wma_close(gp_cds_context);

	wma_wmi_service_close(gp_cds_context);

err_htc_close:
	if (gp_cds_context->htc_ctx) {
		htc_destroy(gp_cds_context->htc_ctx);
		gp_cds_context->htc_ctx = NULL;
	}

err_bmi_close:
	bmi_cleanup(ol_ctx);

err_sched_close:
	cds_sched_close(gp_cds_context);

err_concurrency_lock:
	qdf_mutex_destroy(&cds_ctx->qdf_conc_list_lock);

err_wma_complete_event:
	qdf_event_destroy(&gp_cds_context->wmaCompleteEvent);

err_probe_event:
	qdf_event_destroy(&gp_cds_context->ProbeEvent);

	return QDF_STATUS_E_FAILURE;
} /* cds_open() */

/**
 * cds_pre_enable() - pre enable cds
 * @cds_context: CDS context
 *
 * Return: QDF status
 */
QDF_STATUS cds_pre_enable(v_CONTEXT_t cds_context)
{
	QDF_STATUS qdf_status = QDF_STATUS_SUCCESS;
	p_cds_contextType p_cds_context = (p_cds_contextType) cds_context;
	void *scn;
	void *soc = cds_get_context(QDF_MODULE_ID_SOC);

	QDF_TRACE(QDF_MODULE_ID_SYS, QDF_TRACE_LEVEL_INFO, "cds prestart");
	if (gp_cds_context != p_cds_context) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: Context mismatch", __func__);
		QDF_ASSERT(0);
		return QDF_STATUS_E_INVAL;
	}

	if (p_cds_context->pMACContext == NULL) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: MAC NULL context", __func__);
		QDF_ASSERT(0);
		return QDF_STATUS_E_INVAL;
	}

	if (p_cds_context->pWMAContext == NULL) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: WMA NULL context", __func__);
		QDF_ASSERT(0);
		return QDF_STATUS_E_INVAL;
	}

	scn = cds_get_context(QDF_MODULE_ID_HIF);
	if (!scn) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_FATAL,
			  "%s: scn is null!", __func__);
		return QDF_STATUS_E_FAILURE;
	}

	/* call Packetlog connect service */
	if (QDF_GLOBAL_FTM_MODE != cds_get_conparam() &&
	    QDF_GLOBAL_EPPING_MODE != cds_get_conparam())
		cdp_pkt_log_con_service(soc,
			gp_cds_context->pdev_txrx_ctx,
			scn);

	/* Reset wma wait event */
	qdf_event_reset(&gp_cds_context->wmaCompleteEvent);

	/*call WMA pre start */
	qdf_status = wma_pre_start(gp_cds_context);
	if (!QDF_IS_STATUS_SUCCESS(qdf_status)) {
		QDF_TRACE(QDF_MODULE_ID_SYS, QDF_TRACE_LEVEL_FATAL,
			  "Failed to WMA prestart");
		QDF_ASSERT(0);
		return QDF_STATUS_E_FAILURE;
	}

	/* Need to update time out of complete */
	qdf_status = qdf_wait_single_event(&gp_cds_context->wmaCompleteEvent,
					   CDS_WMA_TIMEOUT);
	if (qdf_status != QDF_STATUS_SUCCESS) {
		if (qdf_status == QDF_STATUS_E_TIMEOUT) {
			QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
				  "%s: Timeout occurred before WMA complete",
				  __func__);
		} else {
			QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
				  "%s: wma_pre_start reporting other error",
				  __func__);
		}
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: Test MC thread by posting a probe message to SYS",
			  __func__);
		wlan_sys_probe();

		QDF_ASSERT(0);
		return QDF_STATUS_E_FAILURE;
	}

	qdf_status = htc_start(gp_cds_context->htc_ctx);
	if (!QDF_IS_STATUS_SUCCESS(qdf_status)) {
		QDF_TRACE(QDF_MODULE_ID_SYS, QDF_TRACE_LEVEL_FATAL,
			  "Failed to Start HTC");
		QDF_ASSERT(0);
		return QDF_STATUS_E_FAILURE;
	}
	qdf_status = wma_wait_for_ready_event(gp_cds_context->pWMAContext);
	if (!QDF_IS_STATUS_SUCCESS(qdf_status)) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_FATAL,
			  "Failed to get ready event from target firmware");
		/*
		 * Panic only if recovery is disabled, else return failure so
		 * that driver load can fail gracefully. We cannot trigger self
		 * recovery here because driver is not fully loaded yet.
		 */
		if (!cds_is_self_recovery_enabled())
			QDF_BUG(0);

		htc_stop(gp_cds_context->htc_ctx);
		return QDF_STATUS_E_FAILURE;
	}

	if (cdp_pdev_post_attach(soc, gp_cds_context->pdev_txrx_ctx)) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_FATAL,
			"Failed to attach pdev");
		htc_stop(gp_cds_context->htc_ctx);
		QDF_ASSERT(0);
		return QDF_STATUS_E_FAILURE;
	}

	return QDF_STATUS_SUCCESS;
}

/**
 * cds_enable() - start/enable cds module
 * @psoc: Psoc pointer
 * @cds_context: CDS context
 *
 * Return: QDF status
 */
QDF_STATUS cds_enable(struct wlan_objmgr_psoc *psoc, v_CONTEXT_t cds_context)
{
	QDF_STATUS qdf_status = QDF_STATUS_SUCCESS;
	tSirRetStatus sirStatus = eSIR_SUCCESS;
	p_cds_contextType p_cds_context = (p_cds_contextType) cds_context;
	tHalMacStartParameters halStartParams;

	QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_INFO,
		  "%s: Starting Libra SW", __func__);

	/* We support only one instance for now ... */
	if (gp_cds_context != p_cds_context) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: mismatch in context", __func__);
		return QDF_STATUS_E_FAILURE;
	}

	if ((p_cds_context->pWMAContext == NULL) ||
	    (p_cds_context->pMACContext == NULL)) {
		if (p_cds_context->pWMAContext == NULL)
			QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
				  "%s: WMA NULL context", __func__);
		else
			QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
				  "%s: MAC NULL context", __func__);

		return QDF_STATUS_E_FAILURE;
	}

	/* Start the wma */
	qdf_status = wma_start(p_cds_context);
	if (qdf_status != QDF_STATUS_SUCCESS) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: Failed to start wma", __func__);
		return QDF_STATUS_E_FAILURE;
	}
	QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_INFO,
		  "%s: wma correctly started", __func__);

	/* Start the MAC */
	qdf_mem_zero(&halStartParams,
		     sizeof(tHalMacStartParameters));

	/* Start the MAC */
	sirStatus =
		mac_start(p_cds_context->pMACContext, &halStartParams);

	if (eSIR_SUCCESS != sirStatus) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_FATAL,
			  "%s: Failed to start MAC", __func__);
		goto err_wma_stop;
	}

	QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_INFO,
		  "%s: MAC correctly started", __func__);

	/* START SME */
	qdf_status = sme_start(p_cds_context->pMACContext);

	if (!QDF_IS_STATUS_SUCCESS(qdf_status)) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_FATAL,
			  "%s: Failed to start SME", __func__);
		goto err_mac_stop;
	}

	QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_INFO,
		  "%s: SME correctly started", __func__);

	if (cdp_soc_attach_target(cds_get_context(QDF_MODULE_ID_SOC))) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_FATAL,
			  "%s: Failed to attach soc target", __func__);
		goto err_sme_stop;
	}

	if (cdp_pdev_attach_target(cds_get_context(QDF_MODULE_ID_SOC),
		(struct cdp_pdev *)cds_get_context(QDF_MODULE_ID_TXRX))) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_FATAL,
			  "%s: Failed to attach pdev target", __func__);
		goto err_soc_target_detach;
	}

	QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_INFO,
		  "TL correctly started");
	QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_INFO,
		  "%s: CDS Start is successful!!", __func__);

	dispatcher_psoc_enable(psoc);

	return QDF_STATUS_SUCCESS;

err_soc_target_detach:
	/* NOOP */

err_sme_stop:
	sme_stop(p_cds_context->pMACContext, HAL_STOP_TYPE_SYS_RESET);

err_mac_stop:
	mac_stop(p_cds_context->pMACContext, HAL_STOP_TYPE_SYS_RESET);

err_wma_stop:
	qdf_event_reset(&(gp_cds_context->wmaCompleteEvent));
	qdf_status = wma_stop(p_cds_context, HAL_STOP_TYPE_RF_KILL);
	if (!QDF_IS_STATUS_SUCCESS(qdf_status)) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: Failed to stop wma", __func__);
		QDF_ASSERT(QDF_IS_STATUS_SUCCESS(qdf_status));
		wma_setneedshutdown(cds_context);
	} else {
		qdf_status =
			qdf_wait_single_event(&(gp_cds_context->wmaCompleteEvent),
					      CDS_WMA_TIMEOUT);
		if (qdf_status != QDF_STATUS_SUCCESS) {
			if (qdf_status == QDF_STATUS_E_TIMEOUT) {
				QDF_TRACE(QDF_MODULE_ID_QDF,
					  QDF_TRACE_LEVEL_FATAL,
					  "%s: Timeout occurred before WMA_stop complete",
					  __func__);
			} else {
				QDF_TRACE(QDF_MODULE_ID_QDF,
					  QDF_TRACE_LEVEL_FATAL,
					  "%s: WMA_stop reporting other error",
					  __func__);
			}
			QDF_ASSERT(0);
			wma_setneedshutdown(cds_context);
		}
	}

	return QDF_STATUS_E_FAILURE;
} /* cds_enable() */

/**
 * cds_disable() - stop/disable cds module
 * @psoc: Psoc pointer
 * @cds_context: CDS context
 *
 * Return: QDF status
 */
QDF_STATUS cds_disable(struct wlan_objmgr_psoc *psoc, v_CONTEXT_t cds_context)
{
	QDF_STATUS qdf_status;
	void *handle;

	/* PSOC disable for all new components. It needs to happen before
	 * target is PDEV suspended such that a component can abort all its
	 * ongoing transaction with FW. Always keep it before wma_stop() as
	 * wma_stop() does target PDEV suspend.
	 */

	dispatcher_psoc_disable(psoc);

	qdf_status = wma_stop(cds_context, HAL_STOP_TYPE_RF_KILL);

	if (!QDF_IS_STATUS_SUCCESS(qdf_status)) {
		cds_err("Failed to stop wma");
		QDF_ASSERT(QDF_IS_STATUS_SUCCESS(qdf_status));
		wma_setneedshutdown(cds_context);
	}

	handle = cds_get_context(QDF_MODULE_ID_PE);
	if (!handle) {
		cds_err("Invalid PE context return!");
		return QDF_STATUS_E_INVAL;
	}
	qdf_status = sme_stop(handle, HAL_STOP_TYPE_SYS_DEEP_SLEEP);
	if (!QDF_IS_STATUS_SUCCESS(qdf_status)) {
		cds_err("Failed to stop SME: %d", qdf_status);
		QDF_ASSERT(QDF_IS_STATUS_SUCCESS(qdf_status));
	}
	qdf_status = mac_stop(handle, HAL_STOP_TYPE_SYS_DEEP_SLEEP);

	if (!QDF_IS_STATUS_SUCCESS(qdf_status)) {
		cds_err("Failed to stop MAC");
		QDF_ASSERT(QDF_IS_STATUS_SUCCESS(qdf_status));
	}

	return qdf_status;
}

#ifdef HIF_USB
static inline void cds_suspend_target(tp_wma_handle wma_handle)
{
	QDF_STATUS status;
	/* Suspend the target and disable interrupt */
	status = pmo_ucfg_psoc_suspend_target(wma_handle->psoc, 0);
	if (status)
		cds_err("Failed to suspend target, status = %d", status);
}
#else
static inline void cds_suspend_target(tp_wma_handle wma_handle)
{
	QDF_STATUS status;
	/* Suspend the target and disable interrupt */
	status = pmo_ucfg_psoc_suspend_target(wma_handle->psoc, 1);
	if (status)
		cds_err("Failed to suspend target, status = %d", status);
}
#endif /* HIF_USB */

/**
 * cds_post_disable() - post disable cds module
 * @cds_context: CDS context
 *
 * Return: QDF status
 */
QDF_STATUS cds_post_disable(v_CONTEXT_t cds_context)
{
	tp_wma_handle wma_handle;
	struct hif_opaque_softc *hif_ctx;
	wma_handle = cds_get_context(QDF_MODULE_ID_WMA);
	if (!wma_handle) {
		cds_err("Failed to get wma_handle!");
		return QDF_STATUS_E_INVAL;
	}

	hif_ctx = cds_get_context(QDF_MODULE_ID_HIF);
	if (!hif_ctx) {
		cds_err("Failed to get hif_handle!");
		return QDF_STATUS_E_INVAL;
	}

	/*
	 * With new state machine changes cds_close can be invoked without
	 * cds_disable. So, send the following clean up prerequisites to fw,
	 * So Fw and host are in sync for cleanup indication:
	 * - Send PDEV_SUSPEND indication to firmware
	 * - Disable HIF Interrupts.
	 * - Clean up CE tasklets.
	 */

	cds_info("send denint sequence to firmware");
	if (!cds_is_driver_recovering())
		cds_suspend_target(wma_handle);
	hif_disable_isr(hif_ctx);
	hif_reset_soc(hif_ctx);

	return QDF_STATUS_SUCCESS;
}

/**
 * cds_close() - close cds module
 * @psoc: Psoc pointer
 * @cds_context: CDS context
 *
 * This API allows user to close modules registered
 * with connectivity device services.
 *
 * Return: QDF status
 */
QDF_STATUS cds_close(struct wlan_objmgr_psoc *psoc, v_CONTEXT_t cds_context)
{
	QDF_STATUS qdf_status;
	void *ctx;

	qdf_status = wma_wmi_work_close(cds_context);
	if (!QDF_IS_STATUS_SUCCESS(qdf_status)) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
		 "%s: Failed to close wma_wmi_work", __func__);
		QDF_ASSERT(0);
	}

	if (gp_cds_context->htc_ctx) {
		htc_stop(gp_cds_context->htc_ctx);
		htc_destroy(gp_cds_context->htc_ctx);
		pmo_ucfg_psoc_update_htc_handle(psoc, NULL);
		gp_cds_context->htc_ctx = NULL;
	}

	ctx = cds_get_context(QDF_MODULE_ID_TXRX);
	cds_set_context(QDF_MODULE_ID_TXRX, NULL);
	cdp_pdev_detach(cds_get_context(QDF_MODULE_ID_SOC),
		       (struct cdp_pdev *)ctx, 1);

	qdf_status = sme_close(((p_cds_contextType) cds_context)->pMACContext);
	if (!QDF_IS_STATUS_SUCCESS(qdf_status)) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: Failed to close SME", __func__);
		QDF_ASSERT(QDF_IS_STATUS_SUCCESS(qdf_status));
	}

	qdf_status = mac_close(((p_cds_contextType) cds_context)->pMACContext);
	if (!QDF_IS_STATUS_SUCCESS(qdf_status)) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: Failed to close MAC", __func__);
		QDF_ASSERT(QDF_IS_STATUS_SUCCESS(qdf_status));
	}

	((p_cds_contextType) cds_context)->pMACContext = NULL;

	cdp_soc_detach(gp_cds_context->dp_soc);

	cds_shutdown_notifier_purge();

	if (true == wma_needshutdown(cds_context)) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
				  "%s: Failed to shutdown wma", __func__);
	} else {
		qdf_status = wma_close(cds_context);
		if (!QDF_IS_STATUS_SUCCESS(qdf_status)) {
			QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
				  "%s: Failed to close wma", __func__);
			QDF_ASSERT(QDF_IS_STATUS_SUCCESS(qdf_status));
		}
	}

	qdf_status = wma_wmi_service_close(cds_context);
	if (!QDF_IS_STATUS_SUCCESS(qdf_status)) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: Failed to close wma_wmi_service", __func__);
		QDF_ASSERT(QDF_IS_STATUS_SUCCESS(qdf_status));
	}

	qdf_status = qdf_event_destroy(&gp_cds_context->wmaCompleteEvent);
	if (!QDF_IS_STATUS_SUCCESS(qdf_status)) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: failed to destroy wmaCompleteEvent", __func__);
		QDF_ASSERT(QDF_IS_STATUS_SUCCESS(qdf_status));
	}

	qdf_status = qdf_event_destroy(&gp_cds_context->ProbeEvent);
	if (!QDF_IS_STATUS_SUCCESS(qdf_status)) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: failed to destroy ProbeEvent", __func__);
		QDF_ASSERT(QDF_IS_STATUS_SUCCESS(qdf_status));
	}

	if (!QDF_IS_STATUS_SUCCESS(qdf_mutex_destroy(
				   &gp_cds_context->qdf_conc_list_lock))) {
		cds_err("Failed to destroy qdf_conc_list_lock");
		QDF_ASSERT(QDF_IS_STATUS_SUCCESS(qdf_status));
	}

	cds_deinit_log_completion();
	cds_deinit_ini_config();
	qdf_timer_module_deinit();

	cds_deregister_all_modules();

	dispatcher_psoc_close(psoc);
	return QDF_STATUS_SUCCESS;
}

/**
 * cds_get_context() - get context data area
 *
 * @moduleId: ID of the module who's context data is being retrived.
 *
 * Each module in the system has a context / data area that is allocated
 * and managed by CDS.  This API allows any user to get a pointer to its
 * allocated context data area from the CDS global context.
 *
 * Return: pointer to the context data area of the module ID
 *	   specified, or NULL if the context data is not allocated for
 *	   the module ID specified
 */
void *cds_get_context(QDF_MODULE_ID moduleId)
{
	void *pModContext = NULL;

	if (gp_cds_context == NULL) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: cds context pointer is null", __func__);
		return NULL;
	}

	switch (moduleId) {
	case QDF_MODULE_ID_HDD:
	{
		pModContext = gp_cds_context->pHDDContext;
		break;
	}

	case QDF_MODULE_ID_SME:
	case QDF_MODULE_ID_PE:
	{
		/* In all these cases, we just return the MAC Context */
		pModContext = gp_cds_context->pMACContext;
		break;
	}

	case QDF_MODULE_ID_WMA:
	{
		/* For wma module */
		pModContext = gp_cds_context->pWMAContext;
		break;
	}

	case QDF_MODULE_ID_QDF:
	{
		/* For SYS this is CDS itself */
		pModContext = gp_cds_context;
		break;
	}

	case QDF_MODULE_ID_HIF:
	{
		pModContext = gp_cds_context->pHIFContext;
		break;
	}

	case QDF_MODULE_ID_HTC:
	{
		pModContext = gp_cds_context->htc_ctx;
		break;
	}

	case QDF_MODULE_ID_QDF_DEVICE:
	{
		pModContext = gp_cds_context->qdf_ctx;
		break;
	}

	case QDF_MODULE_ID_BMI:
	{
		pModContext = gp_cds_context->g_ol_context;
		break;
	}

	case QDF_MODULE_ID_TXRX:
	{
		pModContext = (void *)gp_cds_context->pdev_txrx_ctx;
		break;
	}

	case QDF_MODULE_ID_CFG:
	{
		pModContext = gp_cds_context->cfg_ctx;
		break;
	}

	case QDF_MODULE_ID_SOC:
	{
		pModContext = gp_cds_context->dp_soc;
		break;
	}

	default:
	{
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: Module ID %i does not have its context maintained by CDS",
			  __func__, moduleId);
		QDF_ASSERT(0);
		return NULL;
	}
	}

	if (pModContext == NULL) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: Module ID %i context is Null", __func__,
			  moduleId);
	}

	return pModContext;
} /* cds_get_context() */

/**
 * cds_get_global_context() - get CDS global Context
 *
 * This API allows any user to get the CDS Global Context pointer from a
 * module context data area.
 *
 * Return: pointer to the CDS global context, NULL if the function is
 *	   unable to retreive the CDS context.
 */
v_CONTEXT_t cds_get_global_context(void)
{
	if (gp_cds_context == NULL) {
		/*
		 * To avoid recursive call, this should not change to
		 * QDF_TRACE().
		 */
		pr_err("%s: global cds context is NULL", __func__);
	}

	return gp_cds_context;
} /* cds_get_global_context() */

/**
 * cds_get_driver_state() - Get current driver state
 *
 * This API returns current driver state stored in global context.
 *
 * Return: Driver state enum
 */
enum cds_driver_state cds_get_driver_state(void)
{
	if (gp_cds_context == NULL) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: global cds context is NULL", __func__);

		return CDS_DRIVER_STATE_UNINITIALIZED;
	}

	return gp_cds_context->driver_state;
}

/**
 * cds_set_driver_state() - Set current driver state
 * @state:	Driver state to be set to.
 *
 * This API sets driver state to state. This API only sets the state and doesn't
 * clear states, please make sure to use cds_clear_driver_state to clear any
 * state if required.
 *
 * Return: None
 */
void cds_set_driver_state(enum cds_driver_state state)
{
	if (gp_cds_context == NULL) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: global cds context is NULL: %x", __func__,
			  state);

		return;
	}

	gp_cds_context->driver_state |= state;
}

/**
 * cds_clear_driver_state() - Clear current driver state
 * @state:	Driver state to be cleared.
 *
 * This API clears driver state. This API only clears the state, please make
 * sure to use cds_set_driver_state to set any new states.
 *
 * Return: None
 */
void cds_clear_driver_state(enum cds_driver_state state)
{
	if (gp_cds_context == NULL) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: global cds context is NULL: %x", __func__,
			  state);

		return;
	}

	gp_cds_context->driver_state &= ~state;
}

/**
 * cds_alloc_context() - allocate a context within the CDS global Context
 * @p_cds_context: pointer to the global Vos context
 * @moduleId: module ID who's context area is being allocated.
 * @ppModuleContext: pointer to location where the pointer to the
 *	allocated context is returned. Note this output pointer
 *	is valid only if the API returns QDF_STATUS_SUCCESS
 * @param size: size of the context area to be allocated.
 *
 * This API allows any user to allocate a user context area within the
 * CDS Global Context.
 *
 * Return: QDF status
 */
QDF_STATUS cds_alloc_context(void *p_cds_context, QDF_MODULE_ID moduleID,
			     void **ppModuleContext, uint32_t size)
{
	void **pGpModContext = NULL;

	if (p_cds_context == NULL) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: cds context is null", __func__);
		return QDF_STATUS_E_FAILURE;
	}

	if ((gp_cds_context != p_cds_context) || (ppModuleContext == NULL)) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: context mismatch or null param passed",
			  __func__);
		return QDF_STATUS_E_FAILURE;
	}

	switch (moduleID) {
	case QDF_MODULE_ID_WMA:
	{
		pGpModContext = &(gp_cds_context->pWMAContext);
		break;
	}

	case QDF_MODULE_ID_HIF:
	{
		pGpModContext = &(gp_cds_context->pHIFContext);
		break;
	}

	case QDF_MODULE_ID_BMI:
	{
		pGpModContext = &(gp_cds_context->g_ol_context);
		break;
	}

	case QDF_MODULE_ID_EPPING:
	case QDF_MODULE_ID_SME:
	case QDF_MODULE_ID_PE:
	case QDF_MODULE_ID_HDD:
	case QDF_MODULE_ID_HDD_SOFTAP:
	default:
	{
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: Module ID %i "
			  "does not have its context allocated by CDS",
			  __func__, moduleID);
		QDF_ASSERT(0);
		return QDF_STATUS_E_INVAL;
	}
	}

	if (NULL != *pGpModContext) {
		/* Context has already been allocated!
		 * Prevent double allocation
		 */
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: Module ID %i context has already been allocated",
			  __func__, moduleID);
		return QDF_STATUS_E_EXISTS;
	}

	/* Dynamically allocate the context for module */

	*ppModuleContext = qdf_mem_malloc(size);

	if (*ppModuleContext == NULL) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: Failed to " "allocate Context for module ID %i",
			  __func__, moduleID);
		QDF_ASSERT(0);
		return QDF_STATUS_E_NOMEM;
	}

	*pGpModContext = *ppModuleContext;

	return QDF_STATUS_SUCCESS;
} /* cds_alloc_context() */

/**
 * cds_set_context() - API to set context in global CDS Context
 * @module_id: Module ID
 * @context: Pointer to the Module Context
 *
 * API to set a MODULE Context in global CDS Context
 *
 * Return: QDF_STATUS
 */
QDF_STATUS cds_set_context(QDF_MODULE_ID module_id, void *context)
{
	p_cds_contextType p_cds_context = cds_get_global_context();

	if (!p_cds_context) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "cds context is Invalid");
		return QDF_STATUS_NOT_INITIALIZED;
	}

	switch (module_id) {
	case QDF_MODULE_ID_TXRX:
		p_cds_context->pdev_txrx_ctx = context;
		break;
	case QDF_MODULE_ID_HIF:
		p_cds_context->pHIFContext = context;
		break;
	default:
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: Module ID %i does not have its context managed by CDS",
			  __func__, module_id);
		QDF_ASSERT(0);
		return QDF_STATUS_E_INVAL;
	}

	return QDF_STATUS_SUCCESS;
}

/**
 * cds_free_context() - free an allocated context within the
 *			CDS global Context
 * @p_cds_context: pointer to the global Vos context
 * @moduleId: module ID who's context area is being free
 * @pModuleContext: pointer to module context area to be free'd.
 *
 *  This API allows a user to free the user context area within the
 *  CDS Global Context.
 *
 * Return: QDF status
 */
QDF_STATUS cds_free_context(void *p_cds_context, QDF_MODULE_ID moduleID,
			    void *pModuleContext)
{
	void **pGpModContext = NULL;

	if ((p_cds_context == NULL) || (gp_cds_context != p_cds_context) ||
	    (pModuleContext == NULL)) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: Null params or context mismatch", __func__);
		return QDF_STATUS_E_FAILURE;
	}

	switch (moduleID) {
	case QDF_MODULE_ID_WMA:
	{
		pGpModContext = &(gp_cds_context->pWMAContext);
		break;
	}

	case QDF_MODULE_ID_HIF:
	{
		pGpModContext = &(gp_cds_context->pHIFContext);
		break;
	}

	case QDF_MODULE_ID_TXRX:
	{
		pGpModContext = (void **)&(gp_cds_context->pdev_txrx_ctx);
		break;
	}

	case QDF_MODULE_ID_BMI:
	{
		pGpModContext = &(gp_cds_context->g_ol_context);
		break;
	}

	case QDF_MODULE_ID_EPPING:
	case QDF_MODULE_ID_HDD:
	case QDF_MODULE_ID_SME:
	case QDF_MODULE_ID_PE:
	case QDF_MODULE_ID_HDD_SOFTAP:
	default:
	{
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: Module ID %i "
			  "does not have its context allocated by CDS",
			  __func__, moduleID);
		QDF_ASSERT(0);
		return QDF_STATUS_E_INVAL;
	}
	}

	if (NULL == *pGpModContext) {
		/* Context has not been allocated or freed already! */
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: Module ID %i "
			  "context has not been allocated or freed already",
			  __func__, moduleID);
		return QDF_STATUS_E_FAILURE;
	}

	if (*pGpModContext != pModuleContext) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: pGpModContext != pModuleContext", __func__);
		return QDF_STATUS_E_FAILURE;
	}

	if (pModuleContext != NULL)
		qdf_mem_free(pModuleContext);

	*pGpModContext = NULL;

	return QDF_STATUS_SUCCESS;
} /* cds_free_context() */

/**
 * cds_sys_probe_thread_cback() -  probe mc thread callback
 * @pUserData: pointer to user data
 *
 * Return: none
 */
void cds_sys_probe_thread_cback(void *pUserData)
{
	if (gp_cds_context != pUserData) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: gp_cds_context != pUserData", __func__);
		return;
	}

	if (qdf_event_set(&gp_cds_context->ProbeEvent) != QDF_STATUS_SUCCESS) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: qdf_event_set failed", __func__);
		return;
	}
} /* cds_sys_probe_thread_cback() */

/**
 * cds_wma_complete_cback() - wma complete callback
 * @pUserData: pointer to user data
 *
 * Return: none
 */
void cds_wma_complete_cback(void *pUserData)
{
	if (gp_cds_context != pUserData) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: gp_cds_context != pUserData", __func__);
		return;
	}

	if (qdf_event_set(&gp_cds_context->wmaCompleteEvent) !=
	    QDF_STATUS_SUCCESS) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "%s: qdf_event_set failed", __func__);
		return;
	}
} /* cds_wma_complete_cback() */

/**
 * cds_get_vdev_types() - get vdev type
 * @mode: mode
 * @type: type
 * @sub_type: sub_type
 *
 * Return: WMI vdev type
 */
QDF_STATUS cds_get_vdev_types(enum tQDF_ADAPTER_MODE mode, uint32_t *type,
			      uint32_t *sub_type)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	*type = 0;
	*sub_type = 0;

	switch (mode) {
	case QDF_STA_MODE:
		*type = WMI_VDEV_TYPE_STA;
		break;
	case QDF_SAP_MODE:
		*type = WMI_VDEV_TYPE_AP;
		break;
	case QDF_P2P_DEVICE_MODE:
		*type = WMI_VDEV_TYPE_AP;
		*sub_type = WMI_UNIFIED_VDEV_SUBTYPE_P2P_DEVICE;
		break;
	case QDF_P2P_CLIENT_MODE:
		*type = WMI_VDEV_TYPE_STA;
		*sub_type = WMI_UNIFIED_VDEV_SUBTYPE_P2P_CLIENT;
		break;
	case QDF_P2P_GO_MODE:
		*type = WMI_VDEV_TYPE_AP;
		*sub_type = WMI_UNIFIED_VDEV_SUBTYPE_P2P_GO;
		break;
	case QDF_OCB_MODE:
		*type = WMI_VDEV_TYPE_OCB;
		break;
	case QDF_IBSS_MODE:
		*type = WMI_VDEV_TYPE_IBSS;
		break;
	case QDF_MONITOR_MODE:
		*type = WMI_VDEV_TYPE_MONITOR;
		break;
	case QDF_NDI_MODE:
		*type = WMI_VDEV_TYPE_NDI;
		break;
	default:
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "Invalid device mode %d", mode);
		status = QDF_STATUS_E_INVAL;
		break;
	}
	return status;
}

/**
 * cds_flush_work() - flush pending works
 * @work: pointer to work
 *
 * Return: none
 */
void cds_flush_work(void *work)
{
	cancel_work_sync(work);
}

/**
 * cds_flush_delayed_work() - flush delayed works
 * @dwork: pointer to delayed work
 *
 * Return: none
 */
void cds_flush_delayed_work(void *dwork)
{
	cancel_delayed_work_sync(dwork);
}

/**
 * cds_is_packet_log_enabled() - check if packet log is enabled
 *
 * Return: true if packet log is enabled else false
 */
bool cds_is_packet_log_enabled(void)
{
	hdd_context_t *pHddCtx;

	pHddCtx = (hdd_context_t *) (gp_cds_context->pHDDContext);
	if ((NULL == pHddCtx) || (NULL == pHddCtx->config)) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_FATAL,
			  "%s: Hdd Context is Null", __func__);
		return false;
	}

	return pHddCtx->config->enablePacketLog;
}

/**
 * cds_config_recovery_work() - configure self recovery
 * @qdf_ctx: pointer of qdf context
 *
 * Return: none
 */

static void cds_config_recovery_work(qdf_device_t qdf_ctx)
{
	if (cds_is_driver_recovering()) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			"Recovery is in progress, ignore!");
	} else {
		cds_set_recovery_in_progress(true);
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			"schedule recovery work!");
		pld_schedule_recovery_work(qdf_ctx->dev,
					   PLD_REASON_DEFAULT);
	}
}

/**
 * cds_trigger_recovery() - trigger self recovery
 * @skip_crash_inject: Boolean value to skip to send crash inject cmd
 *
 * Return: none
 */
void cds_trigger_recovery(bool skip_crash_inject)
{
	tp_wma_handle wma_handle = cds_get_context(QDF_MODULE_ID_WMA);
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	qdf_runtime_lock_t recovery_lock;
	qdf_device_t qdf_ctx = cds_get_context(QDF_MODULE_ID_QDF_DEVICE);

	if (!wma_handle) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "WMA context is invalid!");
		return;
	}
	if (!qdf_ctx) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			  "QDF context is invalid!");
		return;
	}

	recovery_lock = qdf_runtime_lock_init("cds_recovery");
	if (!recovery_lock) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			"Could not acquire runtime pm lock!");
		return;
	}

	qdf_runtime_pm_prevent_suspend(recovery_lock);

	if (!skip_crash_inject) {

		wma_crash_inject(wma_handle, RECOVERY_SIM_SELF_RECOVERY, 0);
		status = qdf_wait_single_event(&wma_handle->recovery_event,
			WMA_CRASH_INJECT_TIMEOUT);

		if (QDF_STATUS_SUCCESS != status) {
			QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
				"CRASH_INJECT command is timed out!");
			cds_config_recovery_work(qdf_ctx);
		}
	} else {
		cds_config_recovery_work(qdf_ctx);
	}

	qdf_runtime_pm_allow_suspend(recovery_lock);
	qdf_runtime_lock_deinit(recovery_lock);
}

/**
 * cds_get_monotonic_boottime() - Get kernel boot time.
 *
 * Return: Time in microseconds
 */

uint64_t cds_get_monotonic_boottime(void)
{
	struct timespec ts;

	get_monotonic_boottime(&ts);
	return ((uint64_t) ts.tv_sec * 1000000) + (ts.tv_nsec / 1000);
}

/**
 * cds_set_wakelock_logging() - Logging of wakelock enabled/disabled
 * @value: Boolean value
 *
 * This function is used to set the flag which will indicate whether
 * logging of wakelock is enabled or not
 *
 * Return: None
 */
void cds_set_wakelock_logging(bool value)
{
	p_cds_contextType p_cds_context;

	p_cds_context = cds_get_global_context();
	if (!p_cds_context) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
				"cds context is Invald");
		return;
	}
	p_cds_context->is_wakelock_log_enabled = value;
}

/**
 * cds_is_wakelock_enabled() - Check if logging of wakelock is enabled/disabled
 * @value: Boolean value
 *
 * This function is used to check whether logging of wakelock is enabled or not
 *
 * Return: true if logging of wakelock is enabled
 */
bool cds_is_wakelock_enabled(void)
{
	p_cds_contextType p_cds_context;

	p_cds_context = cds_get_global_context();
	if (!p_cds_context) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
				"cds context is Invald");
		return false;
	}
	return p_cds_context->is_wakelock_log_enabled;
}

/**
 * cds_set_ring_log_level() - Sets the log level of a particular ring
 * @ring_id: ring_id
 * @log_levelvalue: Log level specificed
 *
 * This function converts HLOS values to driver log levels and sets the log
 * level of a particular ring accordingly.
 *
 * Return: None
 */
void cds_set_ring_log_level(uint32_t ring_id, uint32_t log_level)
{
	p_cds_contextType p_cds_context;
	uint32_t log_val;

	p_cds_context = cds_get_global_context();
	if (!p_cds_context) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
				"%s: cds context is Invald", __func__);
		return;
	}

	switch (log_level) {
	case LOG_LEVEL_NO_COLLECTION:
		log_val = WLAN_LOG_LEVEL_OFF;
		break;
	case LOG_LEVEL_NORMAL_COLLECT:
		log_val = WLAN_LOG_LEVEL_NORMAL;
		break;
	case LOG_LEVEL_ISSUE_REPRO:
		log_val = WLAN_LOG_LEVEL_REPRO;
		break;
	case LOG_LEVEL_ACTIVE:
	default:
		log_val = WLAN_LOG_LEVEL_ACTIVE;
		break;
	}

	if (ring_id == RING_ID_WAKELOCK) {
		p_cds_context->wakelock_log_level = log_val;
		return;
	} else if (ring_id == RING_ID_CONNECTIVITY) {
		p_cds_context->connectivity_log_level = log_val;
		return;
	} else if (ring_id == RING_ID_PER_PACKET_STATS) {
		p_cds_context->packet_stats_log_level = log_val;
		return;
	} else if (ring_id == RING_ID_DRIVER_DEBUG) {
		p_cds_context->driver_debug_log_level = log_val;
		return;
	} else if (ring_id == RING_ID_FIRMWARE_DEBUG) {
		p_cds_context->fw_debug_log_level = log_val;
		return;
	}
}

/**
 * cds_get_ring_log_level() - Get the a ring id's log level
 * @ring_id: Ring id
 *
 * Fetch and return the log level corresponding to a ring id
 *
 * Return: Log level corresponding to the ring ID
 */
enum wifi_driver_log_level cds_get_ring_log_level(uint32_t ring_id)
{
	p_cds_contextType p_cds_context;

	p_cds_context = cds_get_global_context();
	if (!p_cds_context) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
				"%s: cds context is Invald", __func__);
		return WLAN_LOG_LEVEL_OFF;
	}

	if (ring_id == RING_ID_WAKELOCK)
		return p_cds_context->wakelock_log_level;
	else if (ring_id == RING_ID_CONNECTIVITY)
		return p_cds_context->connectivity_log_level;
	else if (ring_id == RING_ID_PER_PACKET_STATS)
		return p_cds_context->packet_stats_log_level;
	else if (ring_id == RING_ID_DRIVER_DEBUG)
		return p_cds_context->driver_debug_log_level;
	else if (ring_id == RING_ID_FIRMWARE_DEBUG)
		return p_cds_context->fw_debug_log_level;

	return WLAN_LOG_LEVEL_OFF;
}

/**
 * cds_set_multicast_logging() - Set mutlicast logging value
 * @value: Value of multicast logging
 *
 * Set the multicast logging value which will indicate
 * whether to multicast host and fw messages even
 * without any registration by userspace entity
 *
 * Return: None
 */
void cds_set_multicast_logging(uint8_t value)
{
	cds_multicast_logging = value;
}

/**
 * cds_is_multicast_logging() - Get multicast logging value
 *
 * Get the multicast logging value which will indicate
 * whether to multicast host and fw messages even
 * without any registration by userspace entity
 *
 * Return: 0 - Multicast logging disabled, 1 - Multicast logging enabled
 */
uint8_t cds_is_multicast_logging(void)
{
	return cds_multicast_logging;
}

/*
 * cds_init_log_completion() - Initialize log param structure
 *
 * This function is used to initialize the logging related
 * parameters
 *
 * Return: None
 */
void cds_init_log_completion(void)
{
	p_cds_contextType p_cds_context;

	p_cds_context = cds_get_global_context();
	if (!p_cds_context) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
				"%s: cds context is Invalid", __func__);
		return;
	}

	p_cds_context->log_complete.is_fatal = WLAN_LOG_TYPE_NON_FATAL;
	p_cds_context->log_complete.indicator = WLAN_LOG_INDICATOR_UNUSED;
	p_cds_context->log_complete.reason_code = WLAN_LOG_REASON_CODE_UNUSED;
	p_cds_context->log_complete.is_report_in_progress = false;
	/* Attempting to initialize an already initialized lock
	 * results in a failure. This must be ok here.
	 */
	qdf_spinlock_create(&p_cds_context->bug_report_lock);
}

/**
 * cds_deinit_log_completion() - Deinitialize log param structure
 *
 * This function is used to deinitialize the logging related
 * parameters
 *
 * Return: None
 */
void cds_deinit_log_completion(void)
{
	p_cds_contextType p_cds_context;

	p_cds_context = cds_get_global_context();
	if (!p_cds_context) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
				"%s: cds context is Invalid", __func__);
		return;
	}

	qdf_spinlock_destroy(&p_cds_context->bug_report_lock);
}

/**
 * cds_set_log_completion() - Store the logging params
 * @is_fatal: Indicates if the event triggering bug report is fatal or not
 * @indicator: Source which trigerred the bug report
 * @reason_code: Reason for triggering bug report
 * @recovery_needed: If recovery is needed after bug report
 *
 * This function is used to set the logging parameters based on the
 * caller
 *
 * Return: 0 if setting of params is successful
 */
QDF_STATUS cds_set_log_completion(uint32_t is_fatal,
		uint32_t indicator,
		uint32_t reason_code,
		bool recovery_needed)
{
	p_cds_contextType p_cds_context;

	p_cds_context = cds_get_global_context();
	if (!p_cds_context) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
				"%s: cds context is Invalid", __func__);
		return QDF_STATUS_E_FAILURE;
	}

	qdf_spinlock_acquire(&p_cds_context->bug_report_lock);
	p_cds_context->log_complete.is_fatal = is_fatal;
	p_cds_context->log_complete.indicator = indicator;
	p_cds_context->log_complete.reason_code = reason_code;
	p_cds_context->log_complete.recovery_needed = recovery_needed;
	p_cds_context->log_complete.is_report_in_progress = true;
	qdf_spinlock_release(&p_cds_context->bug_report_lock);
	return QDF_STATUS_SUCCESS;
}

/**
 * cds_get_and_reset_log_completion() - Get and reset logging related params
 * @is_fatal: Indicates if the event triggering bug report is fatal or not
 * @indicator: Source which trigerred the bug report
 * @reason_code: Reason for triggering bug report
 * @recovery_needed: If recovery is needed after bug report
 *
 * This function is used to get the logging related parameters
 *
 * Return: None
 */
void cds_get_and_reset_log_completion(uint32_t *is_fatal,
		uint32_t *indicator,
		uint32_t *reason_code,
		bool *recovery_needed)
{
	p_cds_contextType p_cds_context;

	p_cds_context = cds_get_global_context();
	if (!p_cds_context) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
				"%s: cds context is Invalid", __func__);
		return;
	}

	qdf_spinlock_acquire(&p_cds_context->bug_report_lock);
	*is_fatal =  p_cds_context->log_complete.is_fatal;
	*indicator = p_cds_context->log_complete.indicator;
	*reason_code = p_cds_context->log_complete.reason_code;
	*recovery_needed = p_cds_context->log_complete.recovery_needed;

	/* reset */
	p_cds_context->log_complete.indicator = WLAN_LOG_INDICATOR_UNUSED;
	p_cds_context->log_complete.is_fatal = WLAN_LOG_TYPE_NON_FATAL;
	p_cds_context->log_complete.is_report_in_progress = false;
	p_cds_context->log_complete.reason_code = WLAN_LOG_REASON_CODE_UNUSED;
	p_cds_context->log_complete.recovery_needed = false;
	qdf_spinlock_release(&p_cds_context->bug_report_lock);
}

/**
 * cds_is_log_report_in_progress() - Check if bug reporting is in progress
 *
 * This function is used to check if the bug reporting is already in progress
 *
 * Return: true if the bug reporting is in progress
 */
bool cds_is_log_report_in_progress(void)
{
	p_cds_contextType p_cds_context;

	p_cds_context = cds_get_global_context();
	if (!p_cds_context) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
				"%s: cds context is Invalid", __func__);
		return true;
	}
	return p_cds_context->log_complete.is_report_in_progress;
}

/**
 * cds_is_fatal_event_enabled() - Return if fatal event is enabled
 *
 * Return true if fatal event is enabled.
 */
bool cds_is_fatal_event_enabled(void)
{
	p_cds_contextType p_cds_context;

	p_cds_context = cds_get_global_context();
	if (!p_cds_context) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
				"%s: cds context is Invalid", __func__);
		return false;
	}


	return p_cds_context->enable_fatal_event;
}

/**
 * cds_get_log_indicator() - Get the log flush indicator
 *
 * This function is used to get the log flush indicator
 *
 * Return: log indicator
 */
uint32_t cds_get_log_indicator(void)
{
	p_cds_contextType p_cds_context;
	uint32_t indicator;

	p_cds_context = cds_get_global_context();
	if (!p_cds_context) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
				"%s: cds context is Invalid", __func__);
		return WLAN_LOG_INDICATOR_UNUSED;
	}

	if (cds_is_load_or_unload_in_progress() ||
	    cds_is_driver_recovering()) {
		return WLAN_LOG_INDICATOR_UNUSED;
	}

	qdf_spinlock_acquire(&p_cds_context->bug_report_lock);
	indicator = p_cds_context->log_complete.indicator;
	qdf_spinlock_release(&p_cds_context->bug_report_lock);
	return indicator;
}

/**
 * cds_wlan_flush_host_logs_for_fatal() - Wrapper to flush host logs
 *
 * This function is used to send signal to the logger thread to
 * flush the host logs.
 *
 * Return: None
 *
 */
void cds_wlan_flush_host_logs_for_fatal(void)
{
	wlan_flush_host_logs_for_fatal();
}

/**
 * cds_flush_logs() - Report fatal event to userspace
 * @is_fatal: Indicates if the event triggering bug report is fatal or not
 * @indicator: Source which trigerred the bug report
 * @reason_code: Reason for triggering bug report
 * @dump_mac_trace: If mac trace are needed in logs.
 * @recovery_needed: If recovery is needed after bug report
 *
 * This function sets the log related params and send the WMI command to the
 * FW to flush its logs. On receiving the flush completion event from the FW
 * the same will be conveyed to userspace
 *
 * Return: 0 on success
 */
QDF_STATUS cds_flush_logs(uint32_t is_fatal,
		uint32_t indicator,
		uint32_t reason_code,
		bool dump_mac_trace,
		bool recovery_needed)
{
	uint32_t ret;
	QDF_STATUS status;

	p_cds_contextType p_cds_context;

	p_cds_context = cds_get_global_context();
	if (!p_cds_context) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
				"%s: cds context is Invalid", __func__);
		return QDF_STATUS_E_FAILURE;
	}
	if (!p_cds_context->enable_fatal_event) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
				"%s: Fatal event not enabled", __func__);
		return QDF_STATUS_E_FAILURE;
	}
	if (cds_is_load_or_unload_in_progress() ||
	    cds_is_driver_recovering()) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
				"%s: un/Load/SSR in progress", __func__);
		return QDF_STATUS_E_FAILURE;
	}

	if (cds_is_log_report_in_progress() == true) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
				"%s: Bug report already in progress - dropping! type:%d, indicator=%d reason_code=%d",
				__func__, is_fatal, indicator, reason_code);
		return QDF_STATUS_E_FAILURE;
	}

	status = cds_set_log_completion(is_fatal, indicator,
		reason_code, recovery_needed);
	if (QDF_STATUS_SUCCESS != status) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			"%s: Failed to set log trigger params", __func__);
		return QDF_STATUS_E_FAILURE;
	}

	QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
			"%s: Triggering bug report: type:%d, indicator=%d reason_code=%d",
			__func__, is_fatal, indicator, reason_code);

	if (dump_mac_trace)
		qdf_trace_dump_all(p_cds_context->pMACContext, 0, 0, 500, 0);

	if (WLAN_LOG_INDICATOR_HOST_ONLY == indicator) {
		cds_wlan_flush_host_logs_for_fatal();
		return QDF_STATUS_SUCCESS;
	}

	ret = sme_send_flush_logs_cmd_to_fw(p_cds_context->pMACContext);
	if (0 != ret) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
				"%s: Failed to send flush FW log", __func__);
		cds_init_log_completion();
		return QDF_STATUS_E_FAILURE;
	}

	return QDF_STATUS_SUCCESS;
}

/**
 * cds_logging_set_fw_flush_complete() - Wrapper for FW log flush completion
 *
 * This function is used to send signal to the logger thread to indicate
 * that the flushing of FW logs is complete by the FW
 *
 * Return: None
 *
 */
void cds_logging_set_fw_flush_complete(void)
{
	wlan_logging_set_fw_flush_complete();
}

/**
 * cds_set_fatal_event() - set fatal event status
 * @value: pending statue to set
 *
 * Return: None
 */
void cds_set_fatal_event(bool value)
{
	p_cds_contextType p_cds_context;

	p_cds_context = cds_get_global_context();
	if (!p_cds_context) {
		QDF_TRACE(QDF_MODULE_ID_QDF, QDF_TRACE_LEVEL_ERROR,
				"%s: cds context is Invalid", __func__);
		return;
	}
	p_cds_context->enable_fatal_event = value;
}

/**
 * cds_get_radio_index() - get radio index
 *
 * Return: radio index otherwise, -EINVAL
 */
int cds_get_radio_index(void)
{
	p_cds_contextType p_cds_context;

	p_cds_context = cds_get_global_context();
	if (!p_cds_context) {
		/*
		 * To avoid recursive call, this should not change to
		 * QDF_TRACE().
		 */
		pr_err("%s: cds context is invalid\n", __func__);
		return -EINVAL;
	}

	return p_cds_context->radio_index;
}

/**
 * cds_set_radio_index() - set radio index
 * @radio_index:	the radio index to set
 *
 * Return: QDF status
 */
QDF_STATUS cds_set_radio_index(int radio_index)
{
	p_cds_contextType p_cds_context;

	p_cds_context = cds_get_global_context();
	if (!p_cds_context) {
		pr_err("%s: cds context is invalid\n", __func__);
		return QDF_STATUS_E_FAILURE;
	}

	p_cds_context->radio_index = radio_index;

	return QDF_STATUS_SUCCESS;
}

/**
 * cds_init_ini_config() - API to initialize CDS configuration parameters
 * @cfg: CDS Configuration
 *
 * Return: void
 */

void cds_init_ini_config(struct cds_config_info *cfg)
{
	cds_context_type *cds_ctx;

	cds_ctx = cds_get_context(QDF_MODULE_ID_QDF);
	if (!cds_ctx) {
		cds_err("Invalid CDS Context");
		return;
	}

	cds_ctx->cds_cfg = cfg;
}

/**
 * cds_deinit_ini_config() - API to free CDS configuration parameters
 *
 * Return: void
 */
void cds_deinit_ini_config(void)
{
	cds_context_type *cds_ctx;

	cds_ctx = cds_get_context(QDF_MODULE_ID_QDF);
	if (!cds_ctx) {
		cds_err("Invalid CDS Context");
		return;
	}

	if (cds_ctx->cds_cfg)
		qdf_mem_free(cds_ctx->cds_cfg);

	cds_ctx->cds_cfg = NULL;
}

/**
 * cds_get_ini_config() - API to get CDS configuration parameters
 *
 * Return: cds config structure
 */
struct cds_config_info *cds_get_ini_config(void)
{
	cds_context_type *cds_ctx;

	cds_ctx = cds_get_context(QDF_MODULE_ID_QDF);
	if (!cds_ctx) {
		cds_err("Invalid CDS Context");
		return NULL;
	}

	return cds_ctx->cds_cfg;
}

/**
 * cds_is_5_mhz_enabled() - API to get 5MHZ enabled
 *
 * Return: true if 5 mhz is enabled, false otherwise
 */
bool cds_is_5_mhz_enabled(void)
{
	p_cds_contextType p_cds_context;

	p_cds_context = cds_get_context(QDF_MODULE_ID_QDF);
	if (!p_cds_context) {
		cds_err("%s: cds context is invalid", __func__);
		return false;
	}

	if (p_cds_context->cds_cfg)
		return (p_cds_context->cds_cfg->sub_20_channel_width ==
						WLAN_SUB_20_CH_WIDTH_5);

	return false;
}

/**
 * cds_is_10_mhz_enabled() - API to get 10-MHZ enabled
 *
 * Return: true if 10 mhz is enabled, false otherwise
 */
bool cds_is_10_mhz_enabled(void)
{
	p_cds_contextType p_cds_context;

	p_cds_context = cds_get_context(QDF_MODULE_ID_QDF);
	if (!p_cds_context) {
		cds_err("%s: cds context is invalid", __func__);
		return false;
	}

	if (p_cds_context->cds_cfg)
		return (p_cds_context->cds_cfg->sub_20_channel_width ==
						WLAN_SUB_20_CH_WIDTH_10);

	return false;
}

/**
 * cds_is_sub_20_mhz_enabled() - API to get sub 20-MHZ enabled
 *
 * Return: true if 5 or 10 mhz is enabled, false otherwise
 */
bool cds_is_sub_20_mhz_enabled(void)
{
	p_cds_contextType p_cds_context;

	p_cds_context = cds_get_context(QDF_MODULE_ID_QDF);
	if (!p_cds_context) {
		cds_err("%s: cds context is invalid", __func__);
		return false;
	}

	if (p_cds_context->cds_cfg)
		return p_cds_context->cds_cfg->sub_20_channel_width;

	return false;
}

/**
 * cds_is_self_recovery_enabled() - API to get self recovery enabled
 *
 * Return: true if self recovery enabled, false otherwise
 */
bool cds_is_self_recovery_enabled(void)
{
	p_cds_contextType p_cds_context;

	p_cds_context = cds_get_context(QDF_MODULE_ID_QDF);
	if (!p_cds_context) {
		cds_err("%s: cds context is invalid", __func__);
		return false;
	}

	if (p_cds_context->cds_cfg)
		return p_cds_context->cds_cfg->self_recovery_enabled;

	return false;
}

/**
 * cds_svc_fw_shutdown_ind() - API to send userspace about FW crash
 *
 * @dev: Device Pointer
 *
 * Return: None
 */
void cds_svc_fw_shutdown_ind(struct device *dev)
{
	hdd_svc_fw_shutdown_ind(dev);
}

/*
 * cds_pkt_stats_to_logger_thread() - send pktstats to user
 * @pl_hdr: Pointer to pl_hdr
 * @pkt_dump: Pointer to pkt_dump data structure.
 * @data: Pointer to data
 *
 * This function is used to send the pkt stats to SVC module.
 *
 * Return: None
 */
inline void cds_pkt_stats_to_logger_thread(void *pl_hdr, void *pkt_dump,
						void *data)
{
	if (cds_get_ring_log_level(RING_ID_PER_PACKET_STATS) !=
						WLAN_LOG_LEVEL_ACTIVE)
		return;

	wlan_pkt_stats_to_logger_thread(pl_hdr, pkt_dump, data);
}
