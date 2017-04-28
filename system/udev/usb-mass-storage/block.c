// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "usb-mass-storage.h"

#include <stdio.h>
#include <string.h>

static void ums_block_queue(mx_device_t* device, iotxn_t* txn) {
    ums_block_t* dev = device->ctx;

    if (txn->offset % dev->block_size) {
        iotxn_complete(txn, ERR_INVALID_ARGS, 0);
        return;
    }
    if (txn->length % dev->block_size) {
        iotxn_complete(txn, ERR_INVALID_ARGS, 0);
        return;
    }
    txn->context = dev;

    ums_t* ums = block_to_ums(dev);
    mtx_lock(&ums->iotxn_lock);
    list_add_tail(&ums->queued_iotxns, &txn->node);
    mtx_unlock(&ums->iotxn_lock);
    completion_signal(&ums->iotxn_completion);
}

static void ums_get_info(mx_device_t* device, block_info_t* info) {
    ums_block_t* dev = device->ctx;
    memset(info, 0, sizeof(*info));
    info->block_size = dev->block_size;
    info->block_count = dev->total_blocks;
    info->flags = dev->flags;
}

static ssize_t ums_block_ioctl(mx_device_t* device, uint32_t op, const void* cmd, size_t cmdlen,
                                   void* reply, size_t max) {
    ums_block_t* dev = device->ctx;

    // TODO implement other block ioctls
    switch (op) {
    case IOCTL_BLOCK_GET_INFO: {
        block_info_t* info = reply;
        if (max < sizeof(*info))
            return ERR_BUFFER_TOO_SMALL;
        ums_get_info(device, info);
        return sizeof(*info);
    }
    case IOCTL_DEVICE_SYNC: {
        ums_sync_node_t node;

        ums_t* ums = block_to_ums(dev);
        mtx_lock(&ums->iotxn_lock);
        iotxn_t* txn = list_peek_tail_type(&ums->queued_iotxns, iotxn_t, node);
        if (!txn) {
            txn = ums->curr_txn;
        }
        if (!txn) {
            mtx_unlock(&ums->iotxn_lock);
            return NO_ERROR;
        }
        // queue a stack allocated sync node on ums_t.sync_nodes
        node.iotxn = txn;
        completion_reset(&node.completion);
        list_add_head(&ums->sync_nodes, &node.node);
        mtx_unlock(&ums->iotxn_lock);

        return completion_wait(&node.completion, MX_TIME_INFINITE);
    }
    default:
        return ERR_NOT_SUPPORTED;
    }
}

static mx_off_t ums_block_get_size(mx_device_t* device) {
    ums_block_t* dev = device->ctx;
    return dev->block_size * dev->total_blocks;
}

static mx_status_t ums_block_release(mx_device_t* device) {
    ums_block_t* dev = device->ctx;
    device_destroy(dev->mxdev);
    // ums_block_t is inline with ums_t, so don't try to free it here
    return NO_ERROR;
}

static mx_protocol_device_t ums_block_proto = {
    .iotxn_queue = ums_block_queue,
    .ioctl = ums_block_ioctl,
    .get_size = ums_block_get_size,
    .release = ums_block_release,
};

static void ums_async_set_callbacks(mx_device_t* device, block_callbacks_t* cb) {
    ums_block_t* dev = device->ctx;
    dev->cb = cb;
}

static void ums_async_complete(iotxn_t* txn, void* cookie) {
    ums_block_t* dev = (ums_block_t*)txn->extra[0];
    dev->cb->complete(cookie, txn->status);
    iotxn_release(txn);
}

static void ums_async_read(mx_device_t* device, mx_handle_t vmo, uint64_t length,
                           uint64_t vmo_offset, uint64_t dev_offset, void* cookie) {
    ums_block_t* dev = device->ctx;

    iotxn_t* txn;
    mx_status_t status = iotxn_alloc_vmo(&txn, IOTXN_ALLOC_POOL, vmo, vmo_offset, length);
    if (status != NO_ERROR) {
        dev->cb->complete(cookie, status);
        return;
    }
    txn->opcode = IOTXN_OP_READ;
    txn->offset = dev_offset;
    txn->complete_cb = ums_async_complete;
    txn->cookie = cookie;
    txn->extra[0] = (uintptr_t)dev;
    iotxn_queue(dev->mxdev, txn);
}

static void ums_async_write(mx_device_t* device, mx_handle_t vmo, uint64_t length,
                            uint64_t vmo_offset, uint64_t dev_offset, void* cookie) {
    ums_block_t* dev = device->ctx;

    iotxn_t* txn;
    mx_status_t status = iotxn_alloc_vmo(&txn, IOTXN_ALLOC_POOL, vmo, vmo_offset, length);
    if (status != NO_ERROR) {
        dev->cb->complete(cookie, status);
        return;
    }
    txn->opcode = IOTXN_OP_WRITE;
    txn->offset = dev_offset;
    txn->complete_cb = ums_async_complete;
    txn->cookie = cookie;
    txn->extra[0] = (uintptr_t)dev;
    iotxn_queue(dev->mxdev, txn);
}

static block_ops_t ums_block_ops = {
    .set_callbacks = ums_async_set_callbacks,
    .get_info = ums_get_info,
    .read = ums_async_read,
    .write = ums_async_write,
};

mx_status_t ums_block_add_device(ums_t* ums, ums_block_t* dev) {
    char name[16];
    snprintf(name, sizeof(name), "ums-lun-%02d", dev->lun);
    mx_status_t status = device_create(name, dev, &ums_block_proto, &_driver_usb_mass_storage,
                                       &dev->mxdev);
    if (status != NO_ERROR) {
        return status;
    }
    device_set_protocol(dev->mxdev, MX_PROTOCOL_BLOCK_CORE, &ums_block_ops);
    dev->cb = NULL;

    status = device_add(dev->mxdev, ums->mxdev);
    if (status != NO_ERROR) {
        device_destroy(dev->mxdev);
    }
    return status;
}
