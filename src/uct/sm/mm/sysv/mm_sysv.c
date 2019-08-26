/**
 * Copyright (c) UT-Battelle, LLC. 2014-2015. ALL RIGHTS RESERVED.
 * Copyright (C) Mellanox Technologies Ltd. 2001-2015.  ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <uct/sm/mm/base/mm_md.h>
#include <uct/sm/mm/base/mm_iface.h>
#include <ucs/debug/memtrack.h>
#include <ucs/debug/log.h>
#include <ucs/sys/sys.h>


#define UCT_MM_SYSV_PERM (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP)
#define UCT_MM_SYSV_MSTR (UCT_MM_SYSV_PERM | IPC_CREAT | IPC_EXCL)

typedef struct uct_sysv_packed_rkey {
    uint32_t                shmid;
    uintptr_t               owner_ptr;
} UCS_S_PACKED uct_sysv_packed_rkey_t;

typedef struct uct_sysv_md_config {
    uct_mm_md_config_t      super;
} uct_sysv_md_config_t;

static ucs_config_field_t uct_sysv_md_config_table[] = {
  {"MM_", "", NULL,
   ucs_offsetof(uct_sysv_md_config_t, super), UCS_CONFIG_TYPE_TABLE(uct_mm_md_config_table)},

  {NULL}
};

static ucs_status_t uct_sysv_mem_attach_common(int shmid, void **address_p)
{
    void *address;

    address = shmat(shmid, NULL, 0);
    if (address == MAP_FAILED) {
        ucs_error("shmat(shmid=%d) failed: %m", shmid);
        return UCS_ERR_SHMEM_SEGMENT;
    }

    *address_p = address;
    ucs_trace("attached remote segment %d at address %p", (int)shmid, address);
    return UCS_OK;
}

static ucs_status_t uct_sysv_md_query(uct_md_h md, uct_md_attr_t *md_attr)
{
    uct_mm_md_query(md, md_attr, 1);
    md_attr->rkey_packed_size = sizeof(uct_sysv_packed_rkey_t);
    return UCS_OK;
}

static ucs_status_t
uct_sysv_md_mkey_pack(uct_md_h md, uct_mem_h memh, void *rkey_buffer)
{
    uct_sysv_packed_rkey_t *packed_rkey = rkey_buffer;
    const uct_mm_seg_t     *seg         = memh;

    packed_rkey->shmid     = seg->seg_id;
    packed_rkey->owner_ptr = (uintptr_t)seg->address;
    return UCS_OK;
}

static ucs_status_t
uct_sysv_rkey_unpack(uct_component_t *component, const void *rkey_buffer,
                     uct_rkey_t *rkey_p, void **handle_p)
{
    const uct_sysv_packed_rkey_t *packed_rkey = rkey_buffer;
    ucs_status_t status;
    void *address;

    status = uct_sysv_mem_attach_common(packed_rkey->shmid, &address);
    if (status != UCS_OK) {
        return status;
    }

    *handle_p = address;
    uct_mm_md_make_rkey(address, packed_rkey->owner_ptr, rkey_p);
    return UCS_OK;
}

static ucs_status_t
uct_sysv_rkey_release(uct_component_t *component, uct_rkey_t rkey, void *handle)
{
    void *address = handle;
    return ucs_sysv_free(address);
}

static ucs_status_t
uct_sysv_mem_alloc(uct_mm_md_t *md, uct_mm_seg_t *seg, unsigned uct_flags,
                   const char *alloc_name)
{
    ucs_status_t status;
    int shmid;

    if (!(uct_flags & UCT_MD_MEM_FLAG_FIXED)) {
        seg->address = NULL; // TODO this is not OK; address should be interpreted as a hint
    }

#ifdef SHM_HUGETLB
    if (md->config->hugetlb_mode != UCS_NO) {
        status = ucs_sysv_alloc(&seg->length, seg->length * 2, &seg->address,
                                UCT_MM_SYSV_MSTR | SHM_HUGETLB, alloc_name,
                                &shmid);
        if (status == UCS_OK) {
            seg->seg_id = shmid;
            return UCS_OK;
        }

        ucs_debug("mm failed to allocate %zu bytes with hugetlb", seg->length);
    }
#else
    status = UCS_ERR_UNSUPPORTED;
#endif

    if (md->config->hugetlb_mode != UCS_YES) {
        status = ucs_sysv_alloc(&seg->length, SIZE_MAX, &seg->address,
                                UCT_MM_SYSV_MSTR, alloc_name, &shmid);
        if (status == UCS_OK) {
            seg->seg_id = shmid;
            return UCS_OK;
        }

        ucs_debug("mm failed to allocate %zu bytes without hugetlb", seg->length);
    }

    ucs_error("failed to allocate %zu bytes with mm for %s", seg->length,
              alloc_name);
    return status;
}

static ucs_status_t uct_sysv_iface_mem_free(uct_mm_md_t *md,
                                            const uct_mm_seg_t *seg)
{
    return ucs_sysv_free(seg->address);
}

static ucs_status_t uct_sysv_mem_attach(uct_mm_md_t *md, uct_mm_seg_id_t seg_id,
                                        const void *iface_addr,
                                        uct_mm_remote_seg_t *rseg)
{
    return uct_sysv_mem_attach_common(seg_id, &rseg->address);
}

static void uct_sysv_mem_detach(uct_mm_md_t *md, const uct_mm_remote_seg_t *rseg)
{
    ucs_sysv_free(rseg->address);
}

static uct_mm_md_mapper_ops_t uct_sysv_md_ops = {
   .super = {
        .close                  = uct_mm_md_close,
        .query                  = uct_sysv_md_query,
        .mem_alloc              = uct_mm_md_mem_alloc,
        .mem_free               = uct_mm_md_mem_free,
        .mem_advise             = (void*)ucs_empty_function_return_unsupported,
        .mem_reg                = (void*)ucs_empty_function_return_unsupported,
        .mem_dereg              = (void*)ucs_empty_function_return_unsupported,
        .mkey_pack              = uct_sysv_md_mkey_pack,
        .is_sockaddr_accessible = (void*)ucs_empty_function_return_zero,
        .detect_memory_type     = (void*)ucs_empty_function_return_unsupported
    },
   .query                       = (void*)ucs_empty_function_return_success,
   .iface_addr_length           = (void*)ucs_empty_function_return_zero_int64,
   .iface_addr_pack             = (void*)ucs_empty_function,
   .mem_alloc                  = uct_sysv_mem_alloc,
   .mem_free                   = uct_sysv_iface_mem_free,
   .mem_attach                 = uct_sysv_mem_attach,
   .mem_detach                 = uct_sysv_mem_detach
};

UCT_MM_TL_DEFINE(sysv, &uct_sysv_md_ops, uct_sysv_rkey_unpack,
                 uct_sysv_rkey_release, "SYSV_")
