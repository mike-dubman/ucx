/**
* Copyright (C) Mellanox Technologies Ltd. 2001-2015.  ALL RIGHTS RESERVED.
*
* $COPYRIGHT$
* $HEADER$
*/

#include "cm.h"

#include <uct/api/uct.h>
#include <uct/ib/base/ib_iface.h>
#include <uct/tl/context.h>
#include <ucs/async/async.h>
#include <ucs/debug/log.h>
#include <poll.h>
#include <infiniband/arch.h>


static ucs_config_field_t uct_cm_iface_config_table[] = {
  {"IB_", "RX_INLINE=0", NULL,
   ucs_offsetof(uct_cm_iface_config_t, super), UCS_CONFIG_TYPE_TABLE(uct_ib_iface_config_table)},

  {"ASYNC_MODE", "thread", "Async mode to use",
   ucs_offsetof(uct_cm_iface_config_t, async_mode), UCS_CONFIG_TYPE_ENUM(ucs_async_mode_names)},

  {"TIMEOUT", "100ms", "Timeout for MAD layer",
   ucs_offsetof(uct_cm_iface_config_t, timeout), UCS_CONFIG_TYPE_TIME},

  {"RETRY_COUNT", "20", "Number of retries for MAD layer",
   ucs_offsetof(uct_cm_iface_config_t, retry_count), UCS_CONFIG_TYPE_UINT},

  {NULL}
};

static uct_iface_ops_t uct_cm_iface_ops;


static void uct_cm_iface_notify(uct_cm_iface_t *iface)
{
    ucs_callback_t *cb;
    uct_cm_ep_t *ep;

    while ((iface->inflight == 0) && !ucs_list_is_empty(&iface->notify_list)) {
        ep = ucs_list_extract_head(&iface->notify_list, uct_cm_ep_t, notify.list);
        cb = ep->notify.cb;
        ep->notify.cb = NULL;
        ucs_invoke_callback(cb);
    }
}

ucs_status_t uct_cm_iface_flush(uct_iface_h tl_iface)
{
    uct_cm_iface_t *iface = ucs_derived_of(tl_iface, uct_cm_iface_t);

    if (iface->inflight == 0) {
        return UCS_OK;
    }

    sched_yield();
    return UCS_ERR_NO_RESOURCE;
}

static void uct_cm_iface_handle_sidr_req(uct_cm_iface_t *iface,
                                         struct ib_cm_event *event)
{
    uct_cm_hdr_t *hdr = event->private_data;
    struct ib_cm_sidr_rep_param rep;
    ucs_status_t status;
    void *cm_desc, *desc;
    int ret;

    VALGRIND_MAKE_MEM_DEFINED(hdr, sizeof(hdr));
    VALGRIND_MAKE_MEM_DEFINED(hdr + 1, hdr->length);

    ucs_trace_data("RECV SIDR_REQ am_id %d length %d", hdr->am_id,
                   hdr->length);

    /* Allocate temporary buffer to serve as receive descriptor */
    cm_desc = ucs_malloc(iface->super.config.rx_payload_offset + hdr->length,
                         "cm_recv_desc");
    if (cm_desc == NULL) {
        ucs_error("failed to allocate cm receive descriptor");
        return;
    }

    /* Send reply */
    ucs_trace_data("SEND SIDR_REP (dummy)");
    memset(&rep, 0, sizeof rep);
    rep.status = IB_SIDR_SUCCESS;
    ret = ib_cm_send_sidr_rep(event->cm_id, &rep);
    if (ret) {
        ucs_error("ib_cm_send_sidr_rep() failed: %m");
    }

    /* Call active message handler */
    desc = cm_desc + iface->super.config.rx_headroom_offset;
    status = uct_iface_invoke_am(&iface->super.super, hdr->am_id, hdr + 1,
                                 hdr->length, desc);
    if (status == UCS_OK) {
        ucs_free(cm_desc);
    } else {
        uct_recv_desc_iface(desc) = &iface->super.super.super;
    }
}

static void uct_cm_iface_event_handler(void *arg)
{
    uct_cm_iface_t *iface = arg;
    struct ib_cm_event *event;
    struct ib_cm_id *id;
    int destroy_id;
    int ret;

    ucs_trace_func("");

    for (;;) {
        /* Fetch all events */
        ret = ib_cm_get_event(iface->cmdev, &event);
        if (ret) {
            if (errno != EAGAIN) {
                ucs_warn("ib_cm_get_event() failed: %m");
            }
            return;
        }

        /* Handle the event */
        switch (event->event) {
        case IB_CM_SIDR_REQ_ERROR:
            ucs_error("SIDR request error, status: %s",
                      ibv_wc_status_str(event->param.send_status));
            destroy_id = 1;
            break;
        case IB_CM_SIDR_REQ_RECEIVED:
            uct_cm_iface_handle_sidr_req(iface, event);
            destroy_id = 1; /* Destroy the ID created by the driver */
            break;
        case IB_CM_SIDR_REP_RECEIVED:
            ucs_trace_data("RECV SIDR_REP (dummy)");
            ucs_assert(iface->inflight > 0);
            ucs_atomic_add32(&iface->inflight, -1);
            destroy_id      = 1; /* Destroy the ID which was used for sending */
            break;
        default:
            ucs_warn("Unexpected CM event: %d", event->event);
            destroy_id = 0;
            break;
        }

        /* Acknowledge CM event, remember the id, in case we would destroy it */
        id  = event->cm_id;
        ret = ib_cm_ack_event(event);
        if (ret) {
            ucs_warn("ib_cm_ack_event() failed: %m");
        }

        /* If there is an id which should be destroyed, do it now, after
         * acknowledging all events.
         */
        if (destroy_id) {
            ret = ib_cm_destroy_id(id);
            if (ret) {
                ucs_error("ib_cm_destroy_id() failed: %m");
            }
        }

        uct_cm_iface_notify(iface);
    }
}

static void uct_cm_iface_release_desc(uct_iface_t *tl_iface, void *desc)
{
    uct_cm_iface_t *iface = ucs_derived_of(tl_iface, uct_cm_iface_t);
    ucs_free(desc - iface->super.config.rx_headroom_offset);
}

static UCS_CLASS_INIT_FUNC(uct_cm_iface_t, uct_pd_h pd, uct_worker_h worker,
                           const char *dev_name, size_t rx_headroom,
                           const uct_iface_config_t *tl_config)
{
    uct_cm_iface_config_t *config = ucs_derived_of(tl_config, uct_cm_iface_config_t);
    ucs_status_t status;
    int ret;

    ucs_trace_func("");

    UCS_CLASS_CALL_SUPER_INIT(uct_ib_iface_t, &uct_cm_iface_ops, pd, worker,
                              dev_name, rx_headroom, 0 /* rx_priv_len */,
                              0 /* rx_hdr_len */, 1 /* tx_cq_len */,
                              &config->super);

    self->service_id         = (uint32_t)(ucs_generate_uuid((uintptr_t)self) &
                                            (~IB_CM_ASSIGN_SERVICE_ID_MASK));
    self->inflight           = 0;
    self->config.timeout_ms  = (int)(config->timeout * 1e3 + 0.5);
    self->config.retry_count = ucs_min(config->retry_count, UINT8_MAX);
    ucs_list_head_init(&self->notify_list);

    self->cmdev = ib_cm_open_device(uct_ib_iface_device(&self->super)->ibv_context);
    if (self->cmdev == NULL) {
        ucs_error("ib_cm_open_device() failed: %m. Check if ib_ucm.ko module is loaded.");
        status = UCS_ERR_NO_DEVICE;
        goto err;
    }

    status = ucs_sys_fcntl_modfl(self->cmdev->fd, O_NONBLOCK, 0);
    if (status != UCS_OK) {
        goto err_close_device;
    }

    ret = ib_cm_create_id(self->cmdev, &self->listen_id, self);
    if (ret) {
        ucs_error("ib_cm_create_id() failed: %m");
        status = UCS_ERR_NO_DEVICE;
        goto err_close_device;
    }

    ret = ib_cm_listen(self->listen_id, self->service_id, 0);
    if (ret) {
        ucs_error("ib_cm_listen() failed: %m");
        status = UCS_ERR_INVALID_ADDR;
        goto err_destroy_id;
    }

    if (config->async_mode == UCS_ASYNC_MODE_SIGNAL) {
        ucs_warn("ib_cm fd does not support SIGIO");
    }

    status = ucs_async_set_event_handler(config->async_mode, self->cmdev->fd,
                                         POLLIN, uct_cm_iface_event_handler, self,
                                         worker->async);
    if (status != UCS_OK) {
        ucs_error("failed to set event handler");
        goto err_destroy_id;
    }

    ucs_debug("listening for SIDR service_id 0x%x on fd %d", self->service_id,
              self->cmdev->fd);
    return UCS_OK;

err_destroy_id:
    ib_cm_destroy_id(self->listen_id);
err_close_device:
    ib_cm_close_device(self->cmdev);
err:
    return status;
}

static UCS_CLASS_CLEANUP_FUNC(uct_cm_iface_t)
{
    ucs_trace_func("");

    if (self->inflight) {
        ucs_warn("waiting for %d in-flight requests to complete", self->inflight);
        while (self->inflight) {
            sched_yield();
        }
    }
    ucs_async_unset_event_handler(self->cmdev->fd);
    ib_cm_destroy_id(self->listen_id);
    ib_cm_close_device(self->cmdev);
}

UCS_CLASS_DEFINE(uct_cm_iface_t, uct_ib_iface_t);
static UCS_CLASS_DEFINE_NEW_FUNC(uct_cm_iface_t, uct_iface_t, uct_pd_h, uct_worker_h,
                                 const char*, size_t, const uct_iface_config_t*);
static UCS_CLASS_DEFINE_DELETE_FUNC(uct_cm_iface_t, uct_iface_t);

static ucs_status_t uct_cm_iface_query(uct_iface_h tl_iface,
                                       uct_iface_attr_t *iface_attr)
{
    size_t mtu;

    mtu = ucs_min(IB_CM_SIDR_REQ_PRIVATE_DATA_SIZE - sizeof(uct_cm_hdr_t),
                  UINT8_MAX);

    memset(iface_attr, 0, sizeof(*iface_attr));
    iface_attr->cap.am.max_short      = mtu;
    iface_attr->cap.am.max_bcopy      = mtu;
    iface_attr->cap.am.max_zcopy      = 0;
    iface_attr->iface_addr_len        = sizeof(uct_sockaddr_ib_t);
    iface_attr->ep_addr_len           = 0;
    iface_attr->cap.flags             = UCT_IFACE_FLAG_AM_SHORT |
                                        UCT_IFACE_FLAG_AM_BCOPY |
                                        UCT_IFACE_FLAG_CONNECT_TO_IFACE;
    return UCS_OK;
}

static ucs_status_t uct_cm_iface_get_address(uct_iface_h tl_iface,
                                             struct sockaddr *addr)
{
    uct_cm_iface_t *iface = ucs_derived_of(tl_iface, uct_cm_iface_t);
    uct_sockaddr_ib_t *ib_addr = (uct_sockaddr_ib_t *)addr;

    uct_ib_iface_get_address(&iface->super.super.super, addr);
    ib_addr->id = iface->service_id;
    return UCS_OK;
}


static uct_iface_ops_t uct_cm_iface_ops = {
    .iface_query           = uct_cm_iface_query,
    .iface_flush           = uct_cm_iface_flush,
    .iface_close           = UCS_CLASS_DELETE_FUNC_NAME(uct_cm_iface_t),
    .iface_get_address     = uct_cm_iface_get_address,
    .iface_is_reachable    = uct_ib_iface_is_reachable,
    .ep_create_connected   = UCS_CLASS_NEW_FUNC_NAME(uct_cm_ep_t),
    .ep_destroy            = UCS_CLASS_DELETE_FUNC_NAME(uct_cm_ep_t),
    .iface_release_am_desc = uct_cm_iface_release_desc,
    .ep_am_short           = uct_cm_ep_am_short,
    .ep_am_bcopy           = uct_cm_ep_am_bcopy,
    .ep_req_notify         = uct_cm_ep_req_notify,
    .ep_flush              = uct_cm_ep_flush,
};

static ucs_status_t uct_cm_query_resources(uct_pd_h pd,
                                           uct_tl_resource_desc_t **resources_p,
                                           unsigned *num_resources_p)
{
    return uct_ib_device_query_tl_resources(ucs_derived_of(pd, uct_ib_device_t),
                                            "cm",
                                            0, /* TODO require IB link layer? */
                                            512, /* TODO */
                                            800,
                                            resources_p, num_resources_p);
}

UCT_TL_COMPONENT_DEFINE(uct_cm_tl,
                        uct_cm_query_resources,
                        uct_cm_iface_t,
                        "cm",
                        "CM_",
                        uct_cm_iface_config_table,
                        uct_cm_iface_config_t);
UCT_PD_REGISTER_TL(&uct_ib_pd, &uct_cm_tl);
