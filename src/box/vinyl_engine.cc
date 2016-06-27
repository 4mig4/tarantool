/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "vinyl_engine.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <small/pmatomic.h>

#include "trivia/util.h"
#include "coeio.h"
#include "coio.h"
#include "cfg.h"
#include "scoped_guard.h"

#include "vinyl_index.h"
#include "vinyl_space.h"
#include "xrow.h"
#include "tuple.h"
#include "txn.h"
#include "index.h"
#include "relay.h"
#include "space.h"
#include "schema.h"
#include "port.h"
#include "request.h"
#include "iproto_constants.h"
#include "vinyl.h"

struct cord *worker_pool;
static int worker_pool_size;
static volatile int worker_pool_run;

static inline uint32_t
vinyl_calc_fields(struct key_def *key_def, struct vinyl_field *fields,
		uint32_t *field_count)
{
	/* prepare keys */
	uint32_t size = 0;
	for (uint32_t i = 0; i < key_def->part_count; i++) {
		struct vinyl_field *field = &fields[i];
		assert(field->data != NULL);
		switch (key_def->parts[i].type) {
		case STRING:
			size += mp_sizeof_str(field->size);
			break;
		case NUM:
			size += mp_sizeof_uint(load_u64(field->data));
			break;
		default:
			unreachable();
		}
	}

	uint32_t count = key_def->part_count;
	struct vinyl_field *value_field = &fields[key_def->part_count];
	const char *value = value_field->data;
	const char *valueend = value + value_field->size;
	while (value < valueend) {
		count++;
		mp_next(&value);
	}
	size += mp_sizeof_array(count);
	size += value_field->size;

	*field_count = count;
	return size;
}

static inline char *
vinyl_write_fields(struct key_def *key_def, struct vinyl_field *fields,
		  char *p)
{
	for (uint32_t i = 0; i < key_def->part_count; i++) {
		struct vinyl_field *field = &fields[i];
		switch (key_def->parts[i].type) {
		case STRING:
			p = mp_encode_str(p, field->data, field->size);
			break;
		case NUM:
			p = mp_encode_uint(p, load_u64(field->data));
			break;
		default:
			unreachable();
		}
	}
	struct vinyl_field *value_field = &fields[key_def->part_count];
	memcpy(p, value_field->data, value_field->size);
	return p + value_field->size;
}

struct tuple *
vinyl_convert_tuple(struct vinyl_index *index, struct vinyl_tuple *vinyl_tuple,
		   struct key_def *key_def, struct tuple_format *format)
{
	assert(format);
	assert(key_def->part_count <= BOX_INDEX_PART_MAX);
	struct vinyl_field fields[BOX_INDEX_PART_MAX + 1]; /* parts + value */
	vinyl_tuple_fields(index, vinyl_tuple, fields, key_def->part_count + 1);
	uint32_t field_count = 0;
	size_t size = vinyl_calc_fields(key_def, fields, &field_count);

	struct tuple *tuple = tuple_alloc(format, size);
	char *d = tuple->data;
	d = mp_encode_array(d, field_count);
	d = vinyl_write_fields(key_def, fields, d);
	assert(tuple->data + size == d);
	try {
		tuple_init_field_map(format, tuple);
	} catch (Exception *e) {
		tuple_delete(tuple);
		throw;
	}
	return tuple;
}

static char *
vinyl_convert_tuple_data(struct vinyl_index *index, struct vinyl_tuple *vinyl_tuple,
			struct key_def *key_def,
			uint32_t *bsize)
{
	assert(key_def->part_count <= BOX_INDEX_PART_MAX);
	struct vinyl_field fields[BOX_INDEX_PART_MAX + 1]; /* parts + value */
	vinyl_tuple_fields(index, vinyl_tuple, fields, key_def->part_count + 1);
	uint32_t field_count = 0;
	size_t size = vinyl_calc_fields(key_def, fields, &field_count);
	char *tuple_data = (char *) malloc(size);
	if (tuple_data == NULL)
		tnt_raise(OutOfMemory, size, "malloc", "tuple");
	char *d = tuple_data;
	d = mp_encode_array(d, field_count);
	d = vinyl_write_fields(key_def, fields, d);
	assert(tuple_data + size == d);
	*bsize = size;
	return tuple_data;
}

static void*
vinyl_worker(void *arg)
{
	struct vinyl_env *env = (struct vinyl_env *) arg;
	struct vinyl_service *srv = vinyl_service_new(env);
	if (srv == NULL)
		tnt_raise(OutOfMemory, sizeof(srv), "vinyl", "service");
	while (pm_atomic_load_explicit(&worker_pool_run,
				       pm_memory_order_relaxed)) {
		int rc = vinyl_service_do(srv);
		if (rc == -1)
			break;
		if (rc == 0)
			usleep(10000); /* 10ms */
	}
	vinyl_service_delete(srv);
	return NULL;
}

void
vinyl_workers_start(struct vinyl_env *env)
{
	if (worker_pool_run)
		return;
	/* prepare worker pool */
	worker_pool = NULL;
	worker_pool_size = cfg_geti("vinyl.threads");
	if (worker_pool_size > 0) {
		worker_pool = (struct cord *)calloc(worker_pool_size, sizeof(struct cord));
		if (worker_pool == NULL)
			panic("failed to allocate vinyl worker pool");
	}
	worker_pool_run = 1;
	for (int i = 0; i < worker_pool_size; i++)
		cord_start(&worker_pool[i], "vinyl", vinyl_worker, env);
}

static void
vinyl_workers_stop(void)
{
	if (! worker_pool_run)
		return;
	pm_atomic_store_explicit(&worker_pool_run, 0, pm_memory_order_relaxed);
	for (int i = 0; i < worker_pool_size; i++)
		cord_join(&worker_pool[i]);
	free(worker_pool);
}

int vinyl_info(const char *name, vinyl_info_f cb, void *arg)
{
	VinylEngine *e = (VinylEngine *)engine_find("vinyl");
	struct vinyl_confcursor *cursor = vinyl_confcursor_new(e->env);
	const char *key;
	const char *value;
	if (name) {
		while (vinyl_confcursor_next(cursor, &key, &value) == 0) {
			if (name && strcmp(key, name) != 0)
				continue;
			cb(key, value, arg);
			return 1;
		}
		vinyl_confcursor_delete(cursor);
		return 0;
	}
	while (vinyl_confcursor_next(cursor, &key, &value) == 0) {
		cb(key, value, arg);
	}
	vinyl_confcursor_delete(cursor);
	return 0;
}

static struct mempool vinyl_read_pool;

struct vinyl_read_task {
	struct coio_task base;
	struct vinyl_index *index;
	struct vinyl_cursor *cursor;
	struct vinyl_tx *tx;
	struct vinyl_tuple *key;
	struct vinyl_tuple *result;
};

static ssize_t
vinyl_get_cb(struct coio_task *ptr)
{
	struct vinyl_read_task *task =
		(struct vinyl_read_task *) ptr;
	return vinyl_get(task->tx, task->index, task->key, &task->result, false);
}

static ssize_t
vinyl_index_get_cb(struct coio_task *ptr)
{
	struct vinyl_read_task *task =
		(struct vinyl_read_task *) ptr;
	return vinyl_index_get(task->index, task->key, &task->result, false);
}

static ssize_t
vinyl_cursor_next_cb(struct coio_task *ptr)
{
	struct vinyl_read_task *task =
		(struct vinyl_read_task *) ptr;
	return vinyl_cursor_next(task->cursor, &task->result, false);
}

static ssize_t
vinyl_read_task_free_cb(struct coio_task *ptr)
{
	struct vinyl_read_task *task =
		(struct vinyl_read_task *) ptr;
	if (task->result != NULL)
		vinyl_tuple_unref(task->index, task->result);
	if (task->index != NULL)
		vinyl_index_unref(task->index);
	mempool_free(&vinyl_read_pool, task);
	return 0;
}

static inline int
vinyl_read_task(struct vinyl_index *index, struct vinyl_tx *tx,
	       struct vinyl_cursor *cursor, struct vinyl_tuple *key,
	       struct vinyl_tuple **result,
	       coio_task_cb func)
{
	struct vinyl_read_task *task =
		(struct vinyl_read_task *) mempool_alloc_xc(&vinyl_read_pool);
	task->index = index;
	if (index != NULL)
		vinyl_index_ref(index);
	task->tx = tx;
	task->cursor = cursor;
	task->key = key;
	task->result = NULL;
	if (coio_task(&task->base, func, vinyl_read_task_free_cb,
	              TIMEOUT_INFINITY) == -1) {
		return -1;
	}
	if (index != NULL)
		vinyl_index_unref(index);
	*result = task->result;
	int rc = task->base.base.result; /* save original error code */
	mempool_free(&vinyl_read_pool, task);
	assert(rc == 0 || !diag_is_empty(&fiber()->diag));
	return rc;
}

int
vinyl_index_coget(struct vinyl_index *index, struct vinyl_tuple *key,
		 struct vinyl_tuple **result)
{
	return vinyl_read_task(index, NULL, NULL, key, result, vinyl_index_get_cb);
}

int
vinyl_coget(struct vinyl_tx *tx, struct vinyl_index *index,
	   struct vinyl_tuple *key, struct vinyl_tuple **result)
{
	return vinyl_read_task(index, tx, NULL, key, result, vinyl_get_cb);
}

int
vinyl_cursor_conext(struct vinyl_cursor *cursor, struct vinyl_tuple **result)
{
	return vinyl_read_task(NULL, NULL, cursor, NULL, result,
			      vinyl_cursor_next_cb);
}

VinylEngine::VinylEngine()
	:Engine("vinyl")
	 ,recovery_complete(0)
{
	flags = 0;
	env = NULL;
}

VinylEngine::~VinylEngine()
{
	vinyl_workers_stop();
	if (env)
		vinyl_env_delete(env);
}

void
VinylEngine::init()
{
	worker_pool_run = 0;
	worker_pool_size = 0;
	worker_pool = NULL;
	/* destroyed with cord() */
	mempool_create(&vinyl_read_pool, &cord()->slabc,
	               sizeof(struct vinyl_read_task));
	/* prepare worker pool */
	env = vinyl_env_new();
	if (env == NULL)
		panic("failed to create vinyl environment");
	worker_pool_size = cfg_geti("vinyl.threads");
}

void
VinylEngine::bootstrap()
{
	vinyl_bootstrap(env);
	recovery_complete = 1;
}

void
VinylEngine::beginInitialRecovery()
{
	vinyl_begin_initial_recovery(env);
}

void
VinylEngine::beginFinalRecovery()
{
	vinyl_begin_final_recovery(env);
}

void
VinylEngine::endRecovery()
{
	assert(!recovery_complete);
	/* complete two-phase recovery */
	vinyl_end_recovery(env);
	recovery_complete = 1;
}

Handler *
VinylEngine::open()
{
	return new VinylSpace(this);
}

static inline void
vinyl_send_row(struct xstream *stream, uint32_t space_id, char *tuple,
                uint32_t tuple_size, int64_t lsn)
{
	struct request_replace_body body;
	body.m_body = 0x82; /* map of two elements. */
	body.k_space_id = IPROTO_SPACE_ID;
	body.m_space_id = 0xce; /* uint32 */
	body.v_space_id = mp_bswap_u32(space_id);
	body.k_tuple = IPROTO_TUPLE;
	struct xrow_header row;
	row.type = IPROTO_INSERT;
	row.server_id = 0;
	row.lsn = lsn;
	row.bodycnt = 2;
	row.body[0].iov_base = &body;
	row.body[0].iov_len = sizeof(body);
	row.body[1].iov_base = tuple;
	row.body[1].iov_len = tuple_size;
	xstream_write(stream, &row);
}

struct join_send_space_arg {
	struct vinyl_env *env;
	struct xstream *stream;
};

static void
join_send_space(struct space *sp, void *data)
{
	struct xstream *stream = ((struct join_send_space_arg *) data)->stream;
	if (space_is_temporary(sp))
		return;
	if (!space_is_vinyl(sp))
		return;
	VinylIndex *pk = (VinylIndex *) space_index(sp, 0);
	if (!pk)
		return;

	/* send database */
	struct vinyl_tuple *vinyl_key =
		vinyl_tuple_from_key_data(pk->db, NULL, 0, VINYL_GE);
	if (vinyl_key == NULL)
		diag_raise();
	struct vinyl_cursor *cursor = vinyl_cursor_new(pk->db, vinyl_key, VINYL_GE);
	vinyl_tuple_unref(pk->db, vinyl_key);
	if (cursor == NULL)
		vinyl_raise();
	auto cursor_guard = make_scoped_guard([=]{
		vinyl_cursor_delete(cursor);
	});

	/* tell cursor not to hold a transaction, which
	 * in result enables compaction process
	 * for duplicates */
	vinyl_cursor_set_read_commited(cursor, true);

	while (1) {
		struct vinyl_tuple *vinyl_tuple;
		int rc = vinyl_cursor_next(cursor, &vinyl_tuple, false);
		if (rc != 0)
			diag_raise();
		if (vinyl_tuple == NULL)
			break; /* eof */
		int64_t lsn = vinyl_tuple_lsn(vinyl_tuple);
		uint32_t tuple_size;
		char *tuple = vinyl_convert_tuple_data(pk->db, vinyl_tuple,
			pk->key_def, &tuple_size);
		vinyl_tuple_unref(pk->db, vinyl_tuple);
		try {
			vinyl_send_row(stream, pk->key_def->space_id,
				      tuple, tuple_size, lsn);
		} catch (Exception *e) {
			free(tuple);
			throw;
		}
		free(tuple);
	}
}

/**
 * Relay all data currently stored in Vinyl engine
 * to the replica.
 */
void
VinylEngine::join(struct xstream *stream)
{
	struct join_send_space_arg arg = { env, stream };
	space_foreach(join_send_space, &arg);
}

Index*
VinylEngine::createIndex(struct key_def *key_def)
{
	switch (key_def->type) {
	case TREE: return new VinylIndex(key_def);
	default:
		unreachable();
		return NULL;
	}
}

void
VinylEngine::dropIndex(Index *index)
{
	VinylIndex *i = (VinylIndex *)index;
	/* schedule asynchronous drop */
	int rc = vinyl_index_drop(i->db);
	if (rc == -1)
		vinyl_raise();
	i->db  = NULL;
	i->env = NULL;
}

void
VinylEngine::keydefCheck(struct space *space, struct key_def *key_def)
{
	if (key_def->type != TREE) {
		tnt_raise(ClientError, ER_INDEX_TYPE,
		          key_def->name,
		          space_name(space));
	}
	if (! key_def->opts.is_unique) {
		tnt_raise(ClientError, ER_MODIFY_INDEX,
			  key_def->name,
			  space_name(space),
			  "Vinyl index must be unique");
	}
	if (key_def->iid != 0) {
		tnt_raise(ClientError, ER_MODIFY_INDEX,
			  key_def->name,
			  space_name(space),
			  "Vinyl secondary indexes are not supported");
	}
	unsigned i = 0;
	while (i < key_def->part_count) {
		struct key_part *part = &key_def->parts[i];
		if (part->type != NUM && part->type != STRING) {
			tnt_raise(ClientError, ER_MODIFY_INDEX,
					  key_def->name,
					  space_name(space),
					  "Vinyl index field type must be STR or NUM");
		}
		if (part->fieldno != i) {
			tnt_raise(ClientError, ER_MODIFY_INDEX,
					  key_def->name,
					  space_name(space),
					  "Vinyl key parts must follow first and cannot be sparse");
		}
		i++;
	}
}

void
VinylEngine::begin(struct txn *txn)
{
	assert(txn->engine_tx == NULL);
	txn->engine_tx = vinyl_begin(env);
	if (txn->engine_tx == NULL)
		vinyl_raise();
}

void
VinylEngine::prepare(struct txn *txn)
{
	struct vinyl_tx *tx = (struct vinyl_tx *) txn->engine_tx;

	int rc = vinyl_prepare(tx);
	switch (rc) {
	case 1: /* rollback */
	case 2: /* lock */
		tnt_raise(ClientError, ER_TRANSACTION_CONFLICT);
		break;
	case -1:
		vinyl_raise();
		break;
	}
}

void
VinylEngine::commit(struct txn *txn, int64_t lsn)
{
	struct vinyl_tx *tx = (struct vinyl_tx *) txn->engine_tx;
	if (tx) {
		int rc = vinyl_commit(tx, txn->n_rows ? lsn : 0);
		if (rc == -1) {
			panic("vinyl commit failed: txn->signature = %"
			      PRIu64, lsn);
		}
		txn->engine_tx = NULL;
	}
}

void
VinylEngine::rollback(struct txn *txn)
{
	if (txn->engine_tx == NULL)
		return;

	struct vinyl_tx *tx = (struct vinyl_tx *) txn->engine_tx;
	vinyl_rollback(tx);
	txn->engine_tx = NULL;
}

int
VinylEngine::beginCheckpoint()
{
	/* do not initiate checkpoint during bootstrap,
	 * thread pool is not up yet */
	if (! worker_pool_run)
		return 0;

	int rc = vinyl_checkpoint(env);
	if (rc == -1)
		vinyl_raise();
	return 0;
}

int
VinylEngine::waitCheckpoint(struct vclock*)
{
	if (! worker_pool_run)
		return 0;
	for (;;) {
		if (!vinyl_checkpoint_is_active(env))
			break;
		fiber_yield_timeout(.020);
	}
	return 0;
}