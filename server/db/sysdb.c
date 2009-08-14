/*
   SSSD

   System Database

   Copyright (C) Simo Sorce <ssorce@redhat.com>	2008

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "util/util.h"
#include "db/sysdb_private.h"
#include "confdb/confdb.h"
#include <time.h>

struct ldb_dn *sysdb_user_dn(struct sysdb_ctx *ctx, void *memctx,
                             const char *domain, const char *name)
{
    return ldb_dn_new_fmt(memctx, ctx->ldb, SYSDB_TMPL_USER, name, domain);
}

struct ldb_dn *sysdb_group_dn(struct sysdb_ctx *ctx, void *memctx,
                              const char *domain, const char *name)
{
    return ldb_dn_new_fmt(memctx, ctx->ldb, SYSDB_TMPL_GROUP, name, domain);
}

struct ldb_dn *sysdb_domain_dn(struct sysdb_ctx *ctx, void *memctx,
                              const char *domain)
{
    return ldb_dn_new_fmt(memctx, ctx->ldb, SYSDB_DOM_BASE, domain);
}

struct ldb_context *sysdb_ctx_get_ldb(struct sysdb_ctx *ctx)
{
    return ctx->ldb;
}

struct sysdb_attrs *sysdb_new_attrs(TALLOC_CTX *memctx)
{
    return talloc_zero(memctx, struct sysdb_attrs);
}

int sysdb_attrs_get_el(struct sysdb_attrs *attrs, const char *name,
                       struct ldb_message_element **el)
{
    struct ldb_message_element *e = NULL;
    int i;

    for (i = 0; i < attrs->num; i++) {
        if (strcasecmp(name, attrs->a[i].name) == 0)
            e = &(attrs->a[i]);
    }

    if (!e) {
        e = talloc_realloc(attrs, attrs->a,
                           struct ldb_message_element, attrs->num+1);
        if (!e) return ENOMEM;
        attrs->a = e;

        e[attrs->num].name = talloc_strdup(e, name);
        if (!e[attrs->num].name) return ENOMEM;

        e[attrs->num].num_values = 0;
        e[attrs->num].values = NULL;
        e[attrs->num].flags = 0;

        e = &(attrs->a[attrs->num]);
        attrs->num++;
    }

    *el = e;

    return EOK;
}

int sysdb_attrs_add_val(struct sysdb_attrs *attrs,
                        const char *name, const struct ldb_val *val)
{
    struct ldb_message_element *el = NULL;
    struct ldb_val *vals;
    int ret;

    ret = sysdb_attrs_get_el(attrs, name, &el);

    vals = talloc_realloc(attrs->a, el->values,
                          struct ldb_val, el->num_values+1);
    if (!vals) return ENOMEM;

    vals[el->num_values] = ldb_val_dup(vals, val);
    if (vals[el->num_values].data == NULL &&
        vals[el->num_values].length != 0) {
        return ENOMEM;
    }

    el->values = vals;
    el->num_values++;

    return EOK;
}

int sysdb_attrs_add_string(struct sysdb_attrs *attrs,
                           const char *name, const char *str)
{
    struct ldb_val v;

    v.data = (uint8_t *)discard_const(str);
    v.length = strlen(str);

    return sysdb_attrs_add_val(attrs, name, &v);
}

int sysdb_attrs_steal_string(struct sysdb_attrs *attrs,
                             const char *name, char *str)
{
    struct ldb_message_element *el = NULL;
    struct ldb_val *vals;
    int ret;

    ret = sysdb_attrs_get_el(attrs, name, &el);

    vals = talloc_realloc(attrs->a, el->values,
                          struct ldb_val, el->num_values+1);
    if (!vals) return ENOMEM;
    el->values = vals;

    /* now steal and assign the string */
    talloc_steal(el->values, str);

    el->values[el->num_values].data = (uint8_t *)str;
    el->values[el->num_values].length = strlen(str);
    el->num_values++;

    return EOK;
}

int sysdb_attrs_add_long(struct sysdb_attrs *attrs,
                         const char *name, long value)
{
    struct ldb_val v;
    char *str;
    int ret;

    str = talloc_asprintf(attrs, "%ld", value);
    if (!str) return ENOMEM;

    v.data = (uint8_t *)str;
    v.length = strlen(str);

    ret = sysdb_attrs_add_val(attrs, name, &v);
    talloc_free(str);

    return ret;
}

int sysdb_attrs_add_uint32(struct sysdb_attrs *attrs,
                           const char *name, uint32_t value)
{
    unsigned long val = value;
    struct ldb_val v;
    char *str;
    int ret;

    str = talloc_asprintf(attrs, "%lu", val);
    if (!str) return ENOMEM;

    v.data = (uint8_t *)str;
    v.length = strlen(str);

    ret = sysdb_attrs_add_val(attrs, name, &v);
    talloc_free(str);

    return ret;
}

int sysdb_attrs_add_time_t(struct sysdb_attrs *attrs,
                           const char *name, time_t value)
{
    long long val = value;
    struct ldb_val v;
    char *str;
    int ret;

    str = talloc_asprintf(attrs, "%lld", val);
    if (!str) return ENOMEM;

    v.data = (uint8_t *)str;
    v.length = strlen(str);

    ret = sysdb_attrs_add_val(attrs, name, &v);
    talloc_free(str);

    return ret;
}

/* TODO: make a more complete and precise mapping */
int sysdb_error_to_errno(int ldberr)
{
    switch (ldberr) {
    case LDB_SUCCESS:
        return EOK;
    case LDB_ERR_OPERATIONS_ERROR:
        return EIO;
    case LDB_ERR_NO_SUCH_OBJECT:
        return ENOENT;
    case LDB_ERR_BUSY:
        return EBUSY;
    case LDB_ERR_ENTRY_ALREADY_EXISTS:
        return EEXIST;
    default:
        return EFAULT;
    }
}

/* =Internal-Operations-Queue============================================= */

static void sysdb_run_operation(struct tevent_context *ev,
                                struct tevent_timer *te,
                                struct timeval tv, void *pvt)
{
    struct sysdb_handle *handle = talloc_get_type(pvt, struct sysdb_handle);

    tevent_req_done(handle->subreq);
}

static void sysdb_schedule_operation(struct sysdb_handle *handle)
{
    struct timeval tv = { 0, 0 };
    struct tevent_timer *te;

    te = tevent_add_timer(handle->ctx->ev, handle, tv,
                          sysdb_run_operation, handle);
    if (!te) {
        DEBUG(1, ("Failed to add critical timer to run next handle!\n"));
    }
}

static int sysdb_handle_destructor(void *mem)
{
    struct sysdb_handle *handle = talloc_get_type(mem, struct sysdb_handle);
    bool start_next = false;
    int ret;

    /* if this was the current op start next */
    if (handle->ctx->queue == handle) {
        start_next = true;
    }

    DLIST_REMOVE(handle->ctx->queue, handle);

    if (start_next && handle->ctx->queue) {
        /* run next */
        sysdb_schedule_operation(handle->ctx->queue);
    }

    if (handle->transaction_active) {
        ret = ldb_transaction_cancel(handle->ctx->ldb);
        if (ret != LDB_SUCCESS) {
            DEBUG(1, ("Failed to cancel ldb transaction! (%d)\n", ret));
        }
        /* FIXME: abort() ? */
        handle->transaction_active = false;
    }

    return 0;
}

struct sysdb_get_handle_state {
    struct tevent_context *ev;
    struct sysdb_ctx *ctx;

    struct sysdb_handle *handle;
};

struct tevent_req *sysdb_get_handle_send(TALLOC_CTX *mem_ctx,
                                         struct tevent_context *ev,
                                         struct sysdb_ctx *ctx)
{
    struct tevent_req *req;
    struct sysdb_get_handle_state *state;
    struct sysdb_handle *handle;

    req = tevent_req_create(mem_ctx, &state, struct sysdb_get_handle_state);
    if (!req) return NULL;

    state->ev = ev;
    state->ctx = ctx;

    handle = talloc_zero(state, struct sysdb_handle);
    if (!handle) {
        talloc_zfree(req);
        return NULL;
    }

    handle->ctx = ctx;
    handle->subreq = req;

    talloc_set_destructor((TALLOC_CTX *)handle, sysdb_handle_destructor);

    DLIST_ADD_END(ctx->queue, handle, struct sysdb_handle *);

    if (ctx->queue == handle) {
        /* this is the first in the queue, schedule an immediate run */
        sysdb_schedule_operation(handle);
    }

    state->handle = handle;

    return req;
}

static int sysdb_get_handle_recv(struct tevent_req *req, TALLOC_CTX *memctx,
                                 struct sysdb_handle **handle)
{
    struct sysdb_get_handle_state *state = tevent_req_data(req,
                                             struct sysdb_get_handle_state);
    enum tevent_req_state tstate;
    uint64_t err;

    if (tevent_req_is_error(req, &tstate, &err)) {
        return err;
    }

    *handle = talloc_steal(memctx, state->handle);
    if (!*handle) return ENOMEM;

    return EOK;
}

/* =Transactions========================================================== */

struct sysdb_transaction_state {
    struct tevent_context *ev;
    struct sysdb_ctx *ctx;

    struct sysdb_handle *handle;
};

static void sysdb_transaction_done(struct tevent_req *subreq);

struct tevent_req *sysdb_transaction_send(TALLOC_CTX *mem_ctx,
                                          struct tevent_context *ev,
                                          struct sysdb_ctx *ctx)
{
    struct tevent_req *req, *subreq;
    struct sysdb_transaction_state *state;

    req = tevent_req_create(mem_ctx, &state, struct sysdb_transaction_state);
    if (!req) return NULL;

    state->ev = ev;
    state->ctx = ctx;

    subreq = sysdb_get_handle_send(state, ev, ctx);
    if (!subreq) {
        talloc_zfree(req);
        return NULL;
    }

    tevent_req_set_callback(subreq, sysdb_transaction_done, req);

    return req;
}

static void sysdb_transaction_done(struct tevent_req *subreq)
{
    struct tevent_req *req = tevent_req_callback_data(subreq,
                                                      struct tevent_req);
    struct sysdb_transaction_state *state = tevent_req_data(req,
                                         struct sysdb_transaction_state);
    int ret;

    ret = sysdb_get_handle_recv(subreq, state, &state->handle);
    talloc_zfree(subreq);
    if (ret) {
        tevent_req_error(req, ret);
        return;
    }

    ret = ldb_transaction_start(state->ctx->ldb);
    if (ret != LDB_SUCCESS) {
        DEBUG(1, ("Failed to start ldb transaction! (%d)\n", ret));
        tevent_req_error(req, sysdb_error_to_errno(ret));
        return;
    }
    state->handle->transaction_active = true;

    tevent_req_done(req);
}

int sysdb_transaction_recv(struct tevent_req *req, TALLOC_CTX *memctx,
                           struct sysdb_handle **handle)
{
    struct sysdb_transaction_state *state = tevent_req_data(req,
                                         struct sysdb_transaction_state);
    enum tevent_req_state tstate;
    uint64_t err;

    if (tevent_req_is_error(req, &tstate, &err)) {
        return err;
    }

    *handle = talloc_steal(memctx, state->handle);
    if (!*handle) return ENOMEM;

    return EOK;
}

struct tevent_req *sysdb_transaction_commit_send(TALLOC_CTX *mem_ctx,
                                                 struct tevent_context *ev,
                                                 struct sysdb_handle *handle)
{
    struct tevent_req *req;
    struct sysdb_transaction_state *state;
    int ret;

    req = tevent_req_create(mem_ctx, &state, struct sysdb_transaction_state);
    if (!req) return NULL;

    state->ev = ev;
    state->ctx = handle->ctx;
    state->handle = handle;

    ret = ldb_transaction_commit(handle->ctx->ldb);
    if (ret != LDB_SUCCESS) {
        DEBUG(1, ("Failed to commit ldb transaction! (%d)\n", ret));
        tevent_req_error(req, sysdb_error_to_errno(ret));
    }
    handle->transaction_active = false;

    /* the following may seem weird but it is actually fine.
     * _done() will not actually call the callback as it will not be set
     * until we return. But it will mark the request as done.
     * _post() will trigger the callback as it schedules after we returned
     * and actually set the callback */
    tevent_req_done(req);
    tevent_req_post(req, ev);
    return req;
}

int sysdb_transaction_commit_recv(struct tevent_req *req)
{
    struct sysdb_transaction_state *state = tevent_req_data(req,
                                         struct sysdb_transaction_state);
    enum tevent_req_state tstate;
    uint64_t err;

    /* finally free handle
     * this will also trigger the next transaction in the queue if any */
    talloc_free(state->handle);

    if (tevent_req_is_error(req, &tstate, &err)) {
        return err;
    }

    return EOK;
}

/* default transaction commit receive function.
 * This function does not use the request state so it is safe to use
 * from any caller */
void sysdb_transaction_complete(struct tevent_req *subreq)
{
    struct tevent_req *req = tevent_req_callback_data(subreq,
                                                      struct tevent_req);
    int ret;

    ret = sysdb_transaction_commit_recv(subreq);
    talloc_zfree(subreq);
    if (ret) {
        tevent_req_error(req, ret);
        return;
    }

    tevent_req_done(req);
}


/* =Operations============================================================ */

struct sysdb_operation_state {
    struct tevent_context *ev;
    struct sysdb_ctx *ctx;

    struct sysdb_handle *handle;
};

static void sysdb_operation_process(struct tevent_req *subreq);

struct tevent_req *sysdb_operation_send(TALLOC_CTX *mem_ctx,
                                        struct tevent_context *ev,
                                        struct sysdb_ctx *ctx)
{
    struct tevent_req *req, *subreq;
    struct sysdb_operation_state *state;

    req = tevent_req_create(mem_ctx, &state, struct sysdb_operation_state);
    if (!req) return NULL;

    state->ev = ev;
    state->ctx = ctx;

    subreq = sysdb_get_handle_send(state, ev, ctx);
    if (!subreq) {
        talloc_zfree(req);
        return NULL;
    }

    tevent_req_set_callback(subreq, sysdb_operation_process, req);

    return req;
}

static void sysdb_operation_process(struct tevent_req *subreq)
{
    struct tevent_req *req = tevent_req_callback_data(subreq,
                                                      struct tevent_req);
    struct sysdb_operation_state *state = tevent_req_data(req,
                                         struct sysdb_operation_state);
    int ret;

    ret = sysdb_get_handle_recv(subreq, state, &state->handle);
    talloc_zfree(subreq);
    if (ret) {
        tevent_req_error(req, ret);
        return;
    }

    tevent_req_done(req);
}

int sysdb_operation_recv(struct tevent_req *req, TALLOC_CTX *memctx,
                         struct sysdb_handle **handle)
{
    struct sysdb_operation_state *state = tevent_req_data(req,
                                             struct sysdb_operation_state);
    enum tevent_req_state tstate;
    uint64_t err;

    if (tevent_req_is_error(req, &tstate, &err)) {
        return err;
    }

    *handle = talloc_steal(memctx, state->handle);
    if (!*handle) return ENOMEM;

    return EOK;
}

void sysdb_operation_done(struct sysdb_handle *handle)
{
    talloc_free(handle);
}

/* =Initialization======================================================== */

static int sysdb_read_var(TALLOC_CTX *mem_ctx,
                          struct confdb_ctx *cdb,
                          const char *name,
                          const char *def_value,
                          char **target)
{
    int ret;
    char **values;

    ret = confdb_get_param(cdb, mem_ctx,
                           SYSDB_CONF_SECTION,
                           name, &values);
    if (ret != EOK)
        return ret;

    if (values[0])
        *target = values[0];
    else
        *target = talloc_strdup(mem_ctx, def_value);

    return EOK;
}

static int sysdb_get_db_path(TALLOC_CTX *mem_ctx,
                             struct confdb_ctx *cdb,
                             char **db_path)
{
    TALLOC_CTX *tmp_ctx;
    char *default_ldb_path;
    char *path;
    int ret;

    tmp_ctx = talloc_new(mem_ctx);
    if (!tmp_ctx)
        return ENOMEM;

    default_ldb_path = talloc_asprintf(tmp_ctx, "%s/%s", DB_PATH, SYSDB_FILE);
    if (default_ldb_path == NULL) {
        ret = ENOMEM;
        goto done;
    }

    sysdb_read_var(tmp_ctx, cdb, "ldbFile",
                     default_ldb_path, &path);

    *db_path = talloc_steal(mem_ctx, path);
    ret = EOK;

done:
    talloc_free(tmp_ctx);
    return ret;
}

static int sysdb_check_init(struct sysdb_ctx *ctx)
{
    TALLOC_CTX *tmp_ctx;
    const char *base_ldif;
	struct ldb_ldif *ldif;
    struct ldb_message_element *el;
    struct ldb_result *res;
    struct ldb_dn *verdn;
    char *version = NULL;
    int ret;

    tmp_ctx = talloc_new(ctx);
    if (!tmp_ctx)
        return ENOMEM;

    verdn = ldb_dn_new(tmp_ctx, ctx->ldb, "cn=sysdb");
    if (!verdn) {
        ret = EIO;
        goto done;
    }

    ret = ldb_search(ctx->ldb, tmp_ctx, &res,
                     verdn, LDB_SCOPE_BASE,
                     NULL, NULL);
    if (ret != LDB_SUCCESS) {
        ret = EIO;
        goto done;
    }
    if (res->count > 1) {
        ret = EIO;
        goto done;
    }

    if (res->count == 1) {
        el = ldb_msg_find_element(res->msgs[0], "version");
        if (el) {
            if (el->num_values != 1) {
                ret = EINVAL;
                goto done;
            }
            version = talloc_strndup(tmp_ctx,
                                     (char *)(el->values[0].data),
                                     el->values[0].length);
            if (!version) {
                ret = ENOMEM;
                goto done;
            }

            if (strcmp(version, SYSDB_VERSION) == 0) {
                /* all fine, return */
                ret = EOK;
                goto done;
            }
        }

        DEBUG(0,("Unknown DB version [%s], expected [%s], aborting!\n",
                 version?version:"not found", SYSDB_VERSION));
        ret = EINVAL;
        goto done;
    }

    /* cn=sysdb does not exists, means db is empty, populate */
    base_ldif = SYSDB_BASE_LDIF;
    while ((ldif = ldb_ldif_read_string(ctx->ldb, &base_ldif))) {
        ret = ldb_add(ctx->ldb, ldif->msg);
        if (ret != LDB_SUCCESS) {
            DEBUG(0, ("Failed to inizialiaze DB (%d,[%s]), aborting!\n",
                      ret, ldb_errstring(ctx->ldb)));
            ret = EIO;
            goto done;
        }
        ldb_ldif_read_free(ctx->ldb, ldif);
    }

    ret = EOK;

done:
    talloc_free(tmp_ctx);
    return ret;
}

int sysdb_init(TALLOC_CTX *mem_ctx,
               struct tevent_context *ev,
               struct confdb_ctx *cdb,
               const char *alt_db_path,
               struct sysdb_ctx **_ctx)
{
    struct sysdb_ctx *ctx;
    int ret;

    if (!ev) return EINVAL;

    ctx = talloc_zero(mem_ctx, struct sysdb_ctx);
    if (!ctx) {
        return ENOMEM;
    }
    ctx->ev = ev;

    if (!alt_db_path) {
        ret = sysdb_get_db_path(ctx, cdb, &ctx->ldb_file);
        if (ret != EOK) {
            return ret;
        }
    } else {
        ctx->ldb_file = talloc_strdup(ctx, alt_db_path);
    }
    if (ctx->ldb_file == NULL) {
        return ENOMEM;
    }

    DEBUG(3, ("DB Path is: %s\n", ctx->ldb_file));

    ctx->ldb = ldb_init(ctx, ev);
    if (!ctx->ldb) {
        talloc_free(ctx);
        return EIO;
    }

    ret = ldb_set_debug(ctx->ldb, ldb_debug_messages, NULL);
    if (ret != LDB_SUCCESS) {
        talloc_free(ctx);
        return EIO;
    }

    ret = ldb_connect(ctx->ldb, ctx->ldb_file, 0, NULL);
    if (ret != LDB_SUCCESS) {
        talloc_free(ctx);
        return EIO;
    }

    ret = sysdb_check_init(ctx);
    if (ret != EOK) {
        talloc_free(ctx);
        return ret;
    }

    *_ctx = ctx;

    return EOK;
}
