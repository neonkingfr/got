/*
 * Copyright (c) 2020 Ori Bernstein
 * Copyright (c) 2021 Stefan Sperling <stsp@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/tree.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>

#include <endian.h>
#include <errno.h>
#include <stdint.h>
#include <imsg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sha1.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <zlib.h>

#include "got_error.h"
#include "got_cancel.h"
#include "got_object.h"
#include "got_path.h"
#include "got_reference.h"
#include "got_repository_admin.h"
#include "got_opentemp.h"

#include "got_lib_deltify.h"
#include "got_lib_delta.h"
#include "got_lib_object.h"
#include "got_lib_object_idset.h"
#include "got_lib_object_cache.h"
#include "got_lib_deflate.h"
#include "got_lib_pack.h"
#include "got_lib_pack_create.h"
#include "got_lib_privsep.h"
#include "got_lib_repository.h"
#include "got_lib_ratelimit.h"
#include "got_lib_inflate.h"

#include "murmurhash2.h"

#ifndef MIN
#define	MIN(_a,_b) ((_a) < (_b) ? (_a) : (_b))
#endif

#ifndef MAX
#define	MAX(_a,_b) ((_a) > (_b) ? (_a) : (_b))
#endif

#ifndef nitems
#define nitems(_a)	(sizeof((_a)) / sizeof((_a)[0]))
#endif

struct got_pack_meta {
	struct got_object_id id;
	uint32_t path_hash;
	int	obj_type;
	off_t	size;
	time_t	mtime;

	/* The best delta we picked */
	struct got_pack_meta *head;
	struct got_pack_meta *prev;
	unsigned char *delta_buf; /* if encoded in memory (compressed) */
	off_t	delta_offset;	/* offset in delta cache file (compressed) */
	off_t	delta_len;	/* encoded delta length */
	off_t	delta_compressed_len; /* encoded+compressed delta length */
	int	nchain;

	off_t   reused_delta_offset; /* offset of delta in reused pack file */
	struct got_object_id *base_obj_id;

	/* Only used for delta window */
	struct got_delta_table *dtab;

	/* Only used for writing offset deltas */
	off_t	off;
};

struct got_pack_metavec {
	struct got_pack_meta **meta;
	int nmeta;
	int metasz;
};

static const struct got_error *
alloc_meta(struct got_pack_meta **new, struct got_object_id *id,
    const char *path, int obj_type, time_t mtime, uint32_t seed)
{
	struct got_pack_meta *m;

	*new = NULL;

	m = calloc(1, sizeof(*m));
	if (m == NULL)
		return got_error_from_errno("calloc");

	memcpy(&m->id, id, sizeof(m->id));

	m->path_hash = murmurhash2(path, strlen(path), seed);
	m->obj_type = obj_type;
	m->mtime = mtime;
	*new = m;
	return NULL;
}

static void
clear_meta(struct got_pack_meta *meta)
{
	if (meta == NULL)
		return;
	meta->path_hash = 0;
	free(meta->delta_buf);
	meta->delta_buf = NULL;
	free(meta->base_obj_id);
	meta->base_obj_id = NULL;
	meta->reused_delta_offset = 0;
}

static void
free_nmeta(struct got_pack_meta **meta, int nmeta)
{
	int i;

	for (i = 0; i < nmeta; i++)
		clear_meta(meta[i]);
	free(meta);
}

static int
delta_order_cmp(const void *pa, const void *pb)
{
	struct got_pack_meta *a, *b;

	a = *(struct got_pack_meta **)pa;
	b = *(struct got_pack_meta **)pb;

	if (a->obj_type != b->obj_type)
		return a->obj_type - b->obj_type;
	if (a->path_hash < b->path_hash)
		return -1;
	if (a->path_hash > b->path_hash)
		return 1;
	if (a->mtime < b->mtime)
		return -1;
	if (a->mtime > b->mtime)
		return 1;
	return got_object_id_cmp(&a->id, &b->id);
}

static off_t
delta_size(struct got_delta_instruction *deltas, int ndeltas)
{
	int i;
	off_t size = 32;
	for (i = 0; i < ndeltas; i++) {
		if (deltas[i].copy)
			size += GOT_DELTA_SIZE_SHIFT;
		else
			size += deltas[i].len + 1;
	}
	return size;
}

static const struct got_error *
append(unsigned char **p, size_t *len, off_t *sz, void *seg, int nseg)
{
	char *n;

	if (*len + nseg >= *sz) {
		while (*len + nseg >= *sz)
			*sz += *sz / 2;
		n = realloc(*p, *sz);
		if (n == NULL)
			return got_error_from_errno("realloc");
		*p = n;
	}
	memcpy(*p + *len, seg, nseg);
	*len += nseg;
	return NULL;
}

static const struct got_error *
encode_delta_in_mem(struct got_pack_meta *m, struct got_raw_object *o,
    struct got_delta_instruction *deltas, int ndeltas,
    off_t delta_size, off_t base_size)
{
	const struct got_error *err;
	unsigned char buf[16], *bp;
	int i, j;
	size_t len = 0, compressed_len;
	off_t bufsize = delta_size;
	off_t n;
	struct got_delta_instruction *d;
	uint8_t *delta_buf;

	delta_buf = malloc(bufsize);
	if (delta_buf == NULL)
		return got_error_from_errno("malloc");

	/* base object size */
	buf[0] = base_size & GOT_DELTA_SIZE_VAL_MASK;
	n = base_size >> GOT_DELTA_SIZE_SHIFT;
	for (i = 1; n > 0; i++) {
		buf[i - 1] |= GOT_DELTA_SIZE_MORE;
		buf[i] = n & GOT_DELTA_SIZE_VAL_MASK;
		n >>= GOT_DELTA_SIZE_SHIFT;
	}
	err = append(&delta_buf, &len, &bufsize, buf, i);
	if (err)
		goto done;

	/* target object size */
	buf[0] = o->size & GOT_DELTA_SIZE_VAL_MASK;
	n = o->size >> GOT_DELTA_SIZE_SHIFT;
	for (i = 1; n > 0; i++) {
		buf[i - 1] |= GOT_DELTA_SIZE_MORE;
		buf[i] = n & GOT_DELTA_SIZE_VAL_MASK;
		n >>= GOT_DELTA_SIZE_SHIFT;
	}
	err = append(&delta_buf, &len, &bufsize, buf, i);
	if (err)
		goto done;

	for (j = 0; j < ndeltas; j++) {
		d = &deltas[j];
		if (d->copy) {
			n = d->offset;
			bp = &buf[1];
			buf[0] = GOT_DELTA_BASE_COPY;
			for (i = 0; i < 4; i++) {
				/* DELTA_COPY_OFF1 ... DELTA_COPY_OFF4 */
				buf[0] |= 1 << i;
				*bp++ = n & 0xff;
				n >>= 8;
				if (n == 0)
					break;
			}

			n = d->len;
			if (n != GOT_DELTA_COPY_DEFAULT_LEN) {
				/* DELTA_COPY_LEN1 ... DELTA_COPY_LEN3 */
				for (i = 0; i < 3 && n > 0; i++) {
					buf[0] |= 1 << (i + 4);
					*bp++ = n & 0xff;
					n >>= 8;
				}
			}
			err = append(&delta_buf, &len, &bufsize,
			    buf, bp - buf);
			if (err)
				goto done;
		} else if (o->f == NULL) {
			n = 0;
			while (n != d->len) {
				buf[0] = (d->len - n < 127) ? d->len - n : 127;
				err = append(&delta_buf, &len, &bufsize,
				    buf, 1);
				if (err)
					goto done;
				err = append(&delta_buf, &len, &bufsize,
				    o->data + o->hdrlen + d->offset + n,
				    buf[0]);
				if (err)
					goto done;
				n += buf[0];
			}
		} else {
			char content[128];
			size_t r;
			if (fseeko(o->f, o->hdrlen + d->offset, SEEK_SET) == -1) {
				err = got_error_from_errno("fseeko");
				goto done;
			}
			n = 0;
			while (n != d->len) {
				buf[0] = (d->len - n < 127) ? d->len - n : 127;
				err = append(&delta_buf, &len, &bufsize,
				    buf, 1);
				if (err)
					goto done;
				r = fread(content, 1, buf[0], o->f);
				if (r != buf[0]) {
					err = got_ferror(o->f, GOT_ERR_IO);
					goto done;
				}
				err = append(&delta_buf, &len, &bufsize,
				    content, buf[0]);
				if (err)
					goto done;
				n += buf[0];
			}
		}
	}

	err = got_deflate_to_mem_mmap(&m->delta_buf, &compressed_len,
	    NULL, NULL, delta_buf, 0, len);
	if (err)
		goto done;

	m->delta_len = len;
	m->delta_compressed_len = compressed_len;
done:
	free(delta_buf);
	return err;
}

static const struct got_error *
encode_delta(struct got_pack_meta *m, struct got_raw_object *o,
    struct got_delta_instruction *deltas, int ndeltas,
    off_t base_size, FILE *f)
{
	const struct got_error *err;
	unsigned char buf[16], *bp;
	int i, j;
	off_t n;
	struct got_deflate_buf zb;
	struct got_delta_instruction *d;
	off_t delta_len = 0, compressed_len = 0;

	err = got_deflate_init(&zb, NULL, GOT_DEFLATE_BUFSIZE);
	if (err)
		return err;

	/* base object size */
	buf[0] = base_size & GOT_DELTA_SIZE_VAL_MASK;
	n = base_size >> GOT_DELTA_SIZE_SHIFT;
	for (i = 1; n > 0; i++) {
		buf[i - 1] |= GOT_DELTA_SIZE_MORE;
		buf[i] = n & GOT_DELTA_SIZE_VAL_MASK;
		n >>= GOT_DELTA_SIZE_SHIFT;
	}

	err = got_deflate_append_to_file_mmap(&zb, &compressed_len,
	    buf, 0, i, f, NULL);
	if (err)
		goto done;
	delta_len += i;

	/* target object size */
	buf[0] = o->size & GOT_DELTA_SIZE_VAL_MASK;
	n = o->size >> GOT_DELTA_SIZE_SHIFT;
	for (i = 1; n > 0; i++) {
		buf[i - 1] |= GOT_DELTA_SIZE_MORE;
		buf[i] = n & GOT_DELTA_SIZE_VAL_MASK;
		n >>= GOT_DELTA_SIZE_SHIFT;
	}

	err = got_deflate_append_to_file_mmap(&zb, &compressed_len,
	    buf, 0, i, f, NULL);
	if (err)
		goto done;
	delta_len += i;

	for (j = 0; j < ndeltas; j++) {
		d = &deltas[j];
		if (d->copy) {
			n = d->offset;
			bp = &buf[1];
			buf[0] = GOT_DELTA_BASE_COPY;
			for (i = 0; i < 4; i++) {
				/* DELTA_COPY_OFF1 ... DELTA_COPY_OFF4 */
				buf[0] |= 1 << i;
				*bp++ = n & 0xff;
				n >>= 8;
				if (n == 0)
					break;
			}
			n = d->len;
			if (n != GOT_DELTA_COPY_DEFAULT_LEN) {
				/* DELTA_COPY_LEN1 ... DELTA_COPY_LEN3 */
				for (i = 0; i < 3 && n > 0; i++) {
					buf[0] |= 1 << (i + 4);
					*bp++ = n & 0xff;
					n >>= 8;
				}
			}
			err = got_deflate_append_to_file_mmap(&zb,
			    &compressed_len, buf, 0, bp - buf, f, NULL);
			if (err)
				goto done;
			delta_len += (bp - buf);
		} else if (o->f == NULL) {
			n = 0;
			while (n != d->len) {
				buf[0] = (d->len - n < 127) ? d->len - n : 127;
				err = got_deflate_append_to_file_mmap(&zb,
				    &compressed_len, buf, 0, 1, f, NULL);
				if (err)
					goto done;
				delta_len++;
				err = got_deflate_append_to_file_mmap(&zb,
				    &compressed_len,
				    o->data + o->hdrlen + d->offset + n, 0,
				    buf[0], f, NULL);
				if (err)
					goto done;
				delta_len += buf[0];
				n += buf[0];
			}
		} else {
			char content[128];
			size_t r;
			if (fseeko(o->f, o->hdrlen + d->offset, SEEK_SET) == -1) {
				err = got_error_from_errno("fseeko");
				goto done;
			}
			n = 0;
			while (n != d->len) {
				buf[0] = (d->len - n < 127) ? d->len - n : 127;
				err = got_deflate_append_to_file_mmap(&zb,
				    &compressed_len, buf, 0, 1, f, NULL);
				if (err)
					goto done;
				delta_len++;
				r = fread(content, 1, buf[0], o->f);
				if (r != buf[0]) {
					err = got_ferror(o->f, GOT_ERR_IO);
					goto done;
				}
				err = got_deflate_append_to_file_mmap(&zb,
				    &compressed_len, content, 0, buf[0], f,
				    NULL);
				if (err)
					goto done;
				delta_len += buf[0];
				n += buf[0];
			}
		}
	}

	err = got_deflate_flush(&zb, f, NULL, &compressed_len);
	if (err)
		goto done;

	/* sanity check */
	if (compressed_len != ftello(f) - m->delta_offset) {
		err = got_error(GOT_ERR_COMPRESSION);
		goto done;
	}

	m->delta_len = delta_len;
	m->delta_compressed_len = compressed_len;
done:
	got_deflate_end(&zb);
	return err;
}

static const struct got_error *
report_progress(got_pack_progress_cb progress_cb, void *progress_arg,
    struct got_ratelimit *rl, int ncolored, int nfound, int ntrees,
    off_t packfile_size, int ncommits, int nobj_total, int obj_deltify,
    int nobj_written)
{
	const struct got_error *err;
	int elapsed;

	if (progress_cb == NULL)
		return NULL;

	err = got_ratelimit_check(&elapsed, rl);
	if (err || !elapsed)
		return err;

	return progress_cb(progress_arg, ncolored, nfound, ntrees,
	    packfile_size, ncommits, nobj_total, obj_deltify, nobj_written);
}

static const struct got_error *
add_meta(struct got_pack_meta *m, struct got_pack_metavec *v)
{
	if (v->nmeta == v->metasz){
		size_t newsize = 2 * v->metasz;
		struct got_pack_meta **new;
		new = reallocarray(v->meta, newsize, sizeof(*new));
		if (new == NULL)
			return got_error_from_errno("reallocarray");
		v->meta = new;
		v->metasz = newsize; 
	}

	v->meta[v->nmeta++] = m;
	return NULL;
}

static const struct got_error *
find_pack_for_reuse(struct got_packidx **best_packidx,
    struct got_repository *repo)
{
	const struct got_error *err = NULL;
	struct got_pathlist_entry *pe;
	const char *best_packidx_path = NULL;
	int nobj_max = 0;

	*best_packidx = NULL;

	TAILQ_FOREACH(pe, &repo->packidx_paths, entry) {
		const char *path_packidx = pe->path;
		struct got_packidx *packidx;
		int nobj;

		err = got_repo_get_packidx(&packidx, path_packidx, repo);
		if (err)
			break;

		nobj = be32toh(packidx->hdr.fanout_table[0xff]);
		if (nobj > nobj_max) {
			best_packidx_path = path_packidx;
			nobj_max = nobj;
		}
	}

	if (best_packidx_path) {
		err = got_repo_get_packidx(best_packidx, best_packidx_path,
		    repo);
	}

	return err;
}

struct send_id_arg {
	struct imsgbuf *ibuf;
	struct got_object_id *ids[GOT_IMSG_OBJ_ID_LIST_MAX_NIDS];
	size_t nids;
};

static const struct got_error *
send_id(struct got_object_id *id, void *data, void *arg)
{
	const struct got_error *err = NULL;
	struct send_id_arg *a = arg;

	a->ids[a->nids++] = id;

	if (a->nids >= GOT_IMSG_OBJ_ID_LIST_MAX_NIDS) {
		err = got_privsep_send_object_idlist(a->ibuf, a->ids, a->nids);
		if (err)
			return err;
		a->nids = 0;
	}

	return NULL;
}

static const struct got_error *
recv_reused_delta(struct got_imsg_reused_delta *delta,
    struct got_object_idset *idset, struct got_pack_metavec *v)
{
	struct got_pack_meta *m, *base;

	if (delta->delta_offset + delta->delta_size < delta->delta_offset ||
	    delta->delta_offset +
	    delta->delta_compressed_size < delta->delta_offset)
		return got_error(GOT_ERR_BAD_PACKFILE);

	m = got_object_idset_get(idset, &delta->id);
	if (m == NULL)
		return got_error(GOT_ERR_NO_OBJ);

	base = got_object_idset_get(idset, &delta->base_id);
	if (base == NULL)
		return got_error(GOT_ERR_NO_OBJ);

	m->delta_len = delta->delta_size;
	m->delta_compressed_len = delta->delta_compressed_size;
	m->delta_offset = delta->delta_out_offset;
	m->prev = base;
	m->size = delta->result_size;
	m->reused_delta_offset = delta->delta_offset;
	m->base_obj_id = got_object_id_dup(&delta->base_id);
	if (m->base_obj_id == NULL)
		return got_error_from_errno("got_object_id_dup");

	return add_meta(m, v);
}

static const struct got_error *
search_deltas(struct got_pack_metavec *v, struct got_object_idset *idset,
    int delta_cache_fd, int ncolored, int nfound, int ntrees, int ncommits,
    struct got_repository *repo,
    got_pack_progress_cb progress_cb, void *progress_arg,
    struct got_ratelimit *rl, got_cancel_cb cancel_cb, void *cancel_arg)
{
	const struct got_error *err = NULL;
	struct got_packidx *packidx;
	struct got_pack *pack;
	struct send_id_arg sia;
	struct got_imsg_reused_delta deltas[GOT_IMSG_REUSED_DELTAS_MAX_NDELTAS];
	size_t ndeltas, i;

	err = find_pack_for_reuse(&packidx, repo);
	if (err)
		return err;

	if (packidx == NULL)
		return NULL;

	err = got_object_prepare_delta_reuse(&pack, packidx,
	    delta_cache_fd, repo);
	if (err)
		return err;

	memset(&sia, 0, sizeof(sia));
	sia.ibuf = pack->privsep_child->ibuf;
	err = got_object_idset_for_each(idset, send_id, &sia);
	if (err)
		return err;
	if (sia.nids > 0) {
		err = got_privsep_send_object_idlist(pack->privsep_child->ibuf,
		    sia.ids, sia.nids);
		if (err)
			return err;
	}
	err = got_privsep_send_object_idlist_done(pack->privsep_child->ibuf);
	if (err)
		return err;

	for (;;) {
		int done = 0;

		if (cancel_cb) {
			err = (*cancel_cb)(cancel_arg);
			if (err)
				break;
		}

		err = got_privsep_recv_reused_deltas(&done, deltas, &ndeltas,
		    pack->privsep_child->ibuf);
		if (err || done)
			break;

		for (i = 0; i < ndeltas; i++) {
			struct got_imsg_reused_delta *delta = &deltas[i];
			err = recv_reused_delta(delta, idset, v);
			if (err)
				goto done;
		}

		err = report_progress(progress_cb, progress_arg, rl,
		    ncolored, nfound, ntrees, 0L, ncommits,
		    got_object_idset_num_elements(idset), v->nmeta, 0);
		if (err)
			break;
	}
done:
	return err;
}

static const struct got_error *
pick_deltas(struct got_pack_meta **meta, int nmeta, int ncolored,
    int nfound, int ntrees, int ncommits, int nreused, FILE *delta_cache,
    struct got_repository *repo,
    got_pack_progress_cb progress_cb, void *progress_arg,
    struct got_ratelimit *rl, got_cancel_cb cancel_cb, void *cancel_arg)
{
	const struct got_error *err = NULL;
	struct got_pack_meta *m = NULL, *base = NULL;
	struct got_raw_object *raw = NULL, *base_raw = NULL;
	struct got_delta_instruction *deltas = NULL, *best_deltas = NULL;
	int i, j, ndeltas, best_ndeltas;
	off_t size, best_size;
	const int max_base_candidates = 3;
	size_t delta_memsize = 0;
	const size_t max_delta_memsize = 4 * GOT_DELTA_RESULT_SIZE_CACHED_MAX;
	int outfd = -1;
	uint32_t delta_seed;

	delta_seed = arc4random();

	qsort(meta, nmeta, sizeof(struct got_pack_meta *), delta_order_cmp);
	for (i = 0; i < nmeta; i++) {
		if (cancel_cb) {
			err = (*cancel_cb)(cancel_arg);
			if (err)
				break;
		}
		err = report_progress(progress_cb, progress_arg, rl,
		    ncolored, nfound, ntrees, 0L, ncommits, nreused + nmeta,
		    nreused + i, 0);
		if (err)
			goto done;
		m = meta[i];

		if (m->obj_type == GOT_OBJ_TYPE_COMMIT ||
		    m->obj_type == GOT_OBJ_TYPE_TAG)
			continue;

		err = got_object_raw_open(&raw, &outfd, repo, &m->id);
		if (err)
			goto done;
		m->size = raw->size;

		if (raw->f == NULL) {
			err = got_deltify_init_mem(&m->dtab, raw->data,
			    raw->hdrlen, raw->size + raw->hdrlen, delta_seed);
		} else {
			err = got_deltify_init(&m->dtab, raw->f, raw->hdrlen,
			    raw->size + raw->hdrlen, delta_seed);
		}
		if (err)
			goto done;

		if (i > max_base_candidates) {
			struct got_pack_meta *n = NULL;
			n = meta[i - (max_base_candidates + 1)];
			got_deltify_free(n->dtab);
			n->dtab = NULL;
		}

		best_size = raw->size;
		best_ndeltas = 0;
		for (j = MAX(0, i - max_base_candidates); j < i; j++) {
			if (cancel_cb) {
				err = (*cancel_cb)(cancel_arg);
				if (err)
					goto done;
			}
			base = meta[j];
			/* long chains make unpacking slow, avoid such bases */
			if (base->nchain >= 128 ||
			    base->obj_type != m->obj_type)
				continue;

			err = got_object_raw_open(&base_raw, &outfd, repo,
			    &base->id);
			if (err)
				goto done;

			if (raw->f == NULL && base_raw->f == NULL) {
				err = got_deltify_mem_mem(&deltas, &ndeltas,
				    raw->data, raw->hdrlen,
				    raw->size + raw->hdrlen, delta_seed,
				    base->dtab, base_raw->data,
				    base_raw->hdrlen,
				    base_raw->size + base_raw->hdrlen);
			} else if (raw->f == NULL) {
				err = got_deltify_mem_file(&deltas, &ndeltas,
				    raw->data, raw->hdrlen,
				    raw->size + raw->hdrlen, delta_seed,
				    base->dtab, base_raw->f,
				    base_raw->hdrlen,
				    base_raw->size + base_raw->hdrlen);
			} else if (base_raw->f == NULL) {
				err = got_deltify_file_mem(&deltas, &ndeltas,
				    raw->f, raw->hdrlen,
				    raw->size + raw->hdrlen, delta_seed,
				    base->dtab, base_raw->data,
				    base_raw->hdrlen,
				    base_raw->size + base_raw->hdrlen);
			} else {
				err = got_deltify(&deltas, &ndeltas,
				    raw->f, raw->hdrlen,
				    raw->size + raw->hdrlen, delta_seed,
				    base->dtab, base_raw->f, base_raw->hdrlen,
				    base_raw->size + base_raw->hdrlen);
			}
			got_object_raw_close(base_raw);
			base_raw = NULL;
			if (err)
				goto done;

			size = delta_size(deltas, ndeltas);
			if (size + 32 < best_size){
				/*
				 * if we already picked a best delta,
				 * replace it.
				 */
				best_size = size;
				free(best_deltas);
				best_deltas = deltas;
				best_ndeltas = ndeltas;
				deltas = NULL;
				m->nchain = base->nchain + 1;
				m->prev = base;
				m->head = base->head;
				if (m->head == NULL)
					m->head = base;
			} else {
				free(deltas);
				deltas = NULL;
				ndeltas = 0;
			}
		}

		if (best_ndeltas > 0) {
			if (best_size <= GOT_DELTA_RESULT_SIZE_CACHED_MAX &&
			    delta_memsize + best_size <= max_delta_memsize) {
				delta_memsize += best_size;
				err = encode_delta_in_mem(m, raw, best_deltas,
				    best_ndeltas, best_size, m->prev->size);
			} else {
				m->delta_offset = ftello(delta_cache);
				err = encode_delta(m, raw, best_deltas,
				    best_ndeltas, m->prev->size, delta_cache);
			}
			free(best_deltas);
			best_deltas = NULL;
			best_ndeltas = 0;
			if (err)
				goto done;
		}

		got_object_raw_close(raw);
		raw = NULL;
	}
done:
	for (i = MAX(0, nmeta - max_base_candidates); i < nmeta; i++) {
		got_deltify_free(meta[i]->dtab);
		meta[i]->dtab = NULL;
	}
	if (raw)
		got_object_raw_close(raw);
	if (base_raw)
		got_object_raw_close(base_raw);
	if (outfd != -1 && close(outfd) == -1 && err == NULL)
		err = got_error_from_errno("close");
	free(deltas);
	free(best_deltas);
	return err;
}

static const struct got_error *
search_packidx(int *found, struct got_object_id *id,
    struct got_repository *repo)
{
	const struct got_error *err = NULL;
	struct got_packidx *packidx = NULL;
	int idx;

	*found = 0;

	err = got_repo_search_packidx(&packidx, &idx, repo, id);
	if (err == NULL)
		*found = 1; /* object is already packed */
	else if (err->code == GOT_ERR_NO_OBJ)
		err = NULL;
	return err;
}

static const struct got_error *
add_object(int want_meta, struct got_object_idset *idset,
    struct got_object_id *id, const char *path, int obj_type,
    time_t mtime, uint32_t seed, int loose_obj_only,
    struct got_repository *repo, int *ncolored, int *nfound, int *ntrees,
    got_pack_progress_cb progress_cb, void *progress_arg,
    struct got_ratelimit *rl)
{
	const struct got_error *err;
	struct got_pack_meta *m = NULL;

	if (loose_obj_only) {
		int is_packed;
		err = search_packidx(&is_packed, id, repo);
		if (err)
			return err;
		if (is_packed && want_meta)
			return NULL;
	}

	if (want_meta) {
		err = alloc_meta(&m, id, path, obj_type, mtime, seed);
		if (err)
			return err;

		(*nfound)++;
		err = report_progress(progress_cb, progress_arg, rl,
		    *ncolored, *nfound, *ntrees, 0L, 0, 0, 0, 0);
		if (err) {
			clear_meta(m);
			free(m);
			return err;
		}
	}

	err = got_object_idset_add(idset, id, m);
	if (err) {
		clear_meta(m);
		free(m);
	}
	return err;
}

static const struct got_error *
load_tree_entries(struct got_object_id_queue *ids, int want_meta,
    struct got_object_idset *idset, struct got_object_idset *idset_exclude,
    struct got_object_id *tree_id,
    const char *dpath, time_t mtime, uint32_t seed, struct got_repository *repo,
    int loose_obj_only, int *ncolored, int *nfound, int *ntrees,
    got_pack_progress_cb progress_cb, void *progress_arg,
    struct got_ratelimit *rl, got_cancel_cb cancel_cb, void *cancel_arg)
{
	const struct got_error *err;
	struct got_tree_object *tree;
	char *p = NULL;
	int i;

	err = got_object_open_as_tree(&tree, repo, tree_id);
	if (err)
		return err;

	(*ntrees)++;
	err = report_progress(progress_cb, progress_arg, rl,
	    *ncolored, *nfound, *ntrees, 0L, 0, 0, 0, 0);
	if (err)
		return err;

	for (i = 0; i < got_object_tree_get_nentries(tree); i++) {
		struct got_tree_entry *e = got_object_tree_get_entry(tree, i);
		struct got_object_id *id = got_tree_entry_get_id(e);
		mode_t mode = got_tree_entry_get_mode(e);

		if (cancel_cb) {
			err = (*cancel_cb)(cancel_arg);
			if (err)
				break;
		}

		if (got_object_tree_entry_is_submodule(e) ||
		    got_object_idset_contains(idset, id) ||
		    got_object_idset_contains(idset_exclude, id))
			continue;
		
		if (asprintf(&p, "%s%s%s", dpath, dpath[0] != '\0' ? "/" : "",
		    got_tree_entry_get_name(e)) == -1) {
			err = got_error_from_errno("asprintf");
			break;
		}

		if (S_ISDIR(mode)) {
			struct got_object_qid *qid;
			err = got_object_qid_alloc(&qid, id);
			if (err)
				break;
			qid->data = p;
			p = NULL;
			STAILQ_INSERT_TAIL(ids, qid, entry);
		} else if (S_ISREG(mode) || S_ISLNK(mode)) {
			err = add_object(want_meta,
			    want_meta ? idset : idset_exclude, id, p,
			    GOT_OBJ_TYPE_BLOB, mtime, seed, loose_obj_only,
			    repo, ncolored, nfound, ntrees,
			    progress_cb, progress_arg, rl);
			if (err)
				break;
			free(p);
			p = NULL;
		} else {
			free(p);
			p = NULL;
		}
	}

	got_object_tree_close(tree);
	free(p);
	return err;
}

static const struct got_error *
load_tree(int want_meta, struct got_object_idset *idset,
    struct got_object_idset *idset_exclude,
    struct got_object_id *tree_id, const char *dpath, time_t mtime,
    uint32_t seed, struct got_repository *repo, int loose_obj_only,
    int *ncolored, int *nfound, int *ntrees,
    got_pack_progress_cb progress_cb, void *progress_arg,
    struct got_ratelimit *rl, got_cancel_cb cancel_cb, void *cancel_arg)
{
	const struct got_error *err = NULL;
	struct got_object_id_queue tree_ids;
	struct got_object_qid *qid;

	if (got_object_idset_contains(idset, tree_id) ||
	    got_object_idset_contains(idset_exclude, tree_id))
		return NULL;

	err = got_object_qid_alloc(&qid, tree_id);
	if (err)
		return err;
	qid->data = strdup(dpath);
	if (qid->data == NULL) {
		err = got_error_from_errno("strdup");
		got_object_qid_free(qid);
		return err;
	}

	STAILQ_INIT(&tree_ids);
	STAILQ_INSERT_TAIL(&tree_ids, qid, entry);

	while (!STAILQ_EMPTY(&tree_ids)) {
		const char *path;
		if (cancel_cb) {
			err = (*cancel_cb)(cancel_arg);
			if (err)
				break;
		}

		qid = STAILQ_FIRST(&tree_ids);
		STAILQ_REMOVE_HEAD(&tree_ids, entry);
		path = qid->data;

		if (got_object_idset_contains(idset, &qid->id) ||
		    got_object_idset_contains(idset_exclude, &qid->id)) {
			free(qid->data);
			got_object_qid_free(qid);
			continue;
		}

		err = add_object(want_meta, want_meta ? idset : idset_exclude,
		    &qid->id, path, GOT_OBJ_TYPE_TREE,
		    mtime, seed, loose_obj_only, repo,
		    ncolored, nfound, ntrees, progress_cb, progress_arg, rl);
		if (err) {
			free(qid->data);
			got_object_qid_free(qid);
			break;
		}

		err = load_tree_entries(&tree_ids, want_meta, idset,
		    idset_exclude, &qid->id,
		    path, mtime, seed, repo, loose_obj_only, ncolored, nfound,
		    ntrees, progress_cb, progress_arg, rl,
		    cancel_cb, cancel_arg);
		free(qid->data);
		got_object_qid_free(qid);
		if (err)
			break;
	}

	STAILQ_FOREACH(qid, &tree_ids, entry)
		free(qid->data);
	got_object_id_queue_free(&tree_ids);
	return err;
}

static const struct got_error *
load_commit(int want_meta, struct got_object_idset *idset,
    struct got_object_idset *idset_exclude,
    struct got_object_id *id, struct got_repository *repo, uint32_t seed,
    int loose_obj_only, int *ncolored, int *nfound, int *ntrees,
    got_pack_progress_cb progress_cb, void *progress_arg,
    struct got_ratelimit *rl, got_cancel_cb cancel_cb, void *cancel_arg)
{
	const struct got_error *err;
	struct got_commit_object *commit;

	if (got_object_idset_contains(idset, id) ||
	    got_object_idset_contains(idset_exclude, id))
		return NULL;

	if (loose_obj_only) {
		int is_packed;
		err = search_packidx(&is_packed, id, repo);
		if (err)
			return err;
		if (is_packed && want_meta)
			return NULL;
	}

	err = got_object_open_as_commit(&commit, repo, id);
	if (err)
		return err;

	err = add_object(want_meta, want_meta ? idset : idset_exclude,
	    id, "", GOT_OBJ_TYPE_COMMIT,
	    got_object_commit_get_committer_time(commit), seed,
	    loose_obj_only, repo,
	    ncolored, nfound, ntrees, progress_cb, progress_arg, rl);
	if (err)
		goto done;

	err = load_tree(want_meta, idset, idset_exclude,
	    got_object_commit_get_tree_id(commit),
	    "", got_object_commit_get_committer_time(commit), seed,
	    repo, loose_obj_only, ncolored, nfound, ntrees,
	    progress_cb, progress_arg, rl, cancel_cb, cancel_arg);
done:
	got_object_commit_close(commit);
	return err;
}

static const struct got_error *
load_tag(int want_meta, struct got_object_idset *idset,
    struct got_object_idset *idset_exclude,
    struct got_object_id *id, struct got_repository *repo, uint32_t seed,
    int loose_obj_only, int *ncolored, int *nfound, int *ntrees,
    got_pack_progress_cb progress_cb, void *progress_arg,
    struct got_ratelimit *rl, got_cancel_cb cancel_cb, void *cancel_arg)
{
	const struct got_error *err;
	struct got_tag_object *tag = NULL;

	if (got_object_idset_contains(idset, id) ||
	    got_object_idset_contains(idset_exclude, id))
		return NULL;

	if (loose_obj_only) {
		int is_packed;
		err = search_packidx(&is_packed, id, repo);
		if (err)
			return err;
		if (is_packed && want_meta)
			return NULL;
	}

	err = got_object_open_as_tag(&tag, repo, id);
	if (err)
		return err;

	err = add_object(want_meta, want_meta ? idset : idset_exclude,
	    id, "", GOT_OBJ_TYPE_TAG,
	    got_object_tag_get_tagger_time(tag), seed, loose_obj_only, repo,
	    ncolored, nfound, ntrees, progress_cb, progress_arg, rl);
	if (err)
		goto done;

	switch (got_object_tag_get_object_type(tag)) {
	case GOT_OBJ_TYPE_COMMIT:
		err = load_commit(want_meta, idset, idset_exclude,
		    got_object_tag_get_object_id(tag), repo, seed,
		    loose_obj_only, ncolored, nfound, ntrees,
		    progress_cb, progress_arg, rl, cancel_cb, cancel_arg);
		break;
	case GOT_OBJ_TYPE_TREE:
		err = load_tree(want_meta, idset, idset_exclude,
		    got_object_tag_get_object_id(tag), "",
		    got_object_tag_get_tagger_time(tag), seed, repo,
		    loose_obj_only, ncolored, nfound, ntrees,
		    progress_cb, progress_arg, rl, cancel_cb, cancel_arg);
		break;
	default:
		break;
	}

done:
	got_object_tag_close(tag);
	return err;
}

enum findtwixt_color {
	COLOR_KEEP = 0,
	COLOR_DROP,
	COLOR_BLANK,
	COLOR_SKIP,
};

static const int findtwixt_colors[] = {
	COLOR_KEEP,
	COLOR_DROP,
	COLOR_BLANK,
	COLOR_SKIP,
};

static const struct got_error *
paint_commit(struct got_object_qid *qid, int color)
{
	if (color < 0 || color >= nitems(findtwixt_colors))
		return got_error(GOT_ERR_RANGE);

	qid->data = (void *)&findtwixt_colors[color];
	return NULL;
}

static const struct got_error *
queue_commit_id(struct got_object_id_queue *ids, struct got_object_id *id,
    int color, struct got_repository *repo)
{
	const struct got_error *err;
	struct got_object_qid *qid;

	err = got_object_qid_alloc(&qid, id);
	if (err)
		return err;

	STAILQ_INSERT_TAIL(ids, qid, entry);
	return paint_commit(qid, color);
}

struct append_id_arg {
	struct got_object_id **array;
	int idx;
	struct got_object_idset *drop;
	struct got_object_idset *skip;
};

static const struct got_error *
append_id(struct got_object_id *id, void *data, void *arg)
{
	struct append_id_arg *a = arg;

	if (got_object_idset_contains(a->skip, id) ||
	    got_object_idset_contains(a->drop, id))
		return NULL;

	a->array[++a->idx] = got_object_id_dup(id);
	if (a->array[a->idx] == NULL)
		return got_error_from_errno("got_object_id_dup");

	return NULL;
}

static const struct got_error *
queue_commit_or_tag_id(struct got_object_id *id, int color,
    struct got_object_id_queue *ids, struct got_repository *repo)
{
	const struct got_error *err;
	struct got_tag_object *tag = NULL;
	int obj_type;

	err = got_object_get_type(&obj_type, repo, id);
	if (err)
		return err;

	if (obj_type == GOT_OBJ_TYPE_TAG) {
		err = got_object_open_as_tag(&tag, repo, id);
		if (err)
			return err;
		obj_type = got_object_tag_get_object_type(tag);
		id = got_object_tag_get_object_id(tag);
	}

	if (obj_type == GOT_OBJ_TYPE_COMMIT) {
		err = queue_commit_id(ids, id, color, repo);
		if (err)
			goto done;
	}
done:
	if (tag)
		got_object_tag_close(tag);
	return err;
}

static const struct got_error *
paint_commits(int *ncolored, struct got_object_id_queue *ids, int nids,
    struct got_object_idset *keep, struct got_object_idset *drop,
    struct got_object_idset *skip, struct got_repository *repo,
    got_pack_progress_cb progress_cb, void *progress_arg,
    struct got_ratelimit *rl, got_cancel_cb cancel_cb, void *cancel_arg)
{
	const struct got_error *err = NULL;
	struct got_commit_object *commit = NULL;
	const struct got_object_id_queue *parents;
	struct got_object_qid *qid;
	int nqueued = nids, nskip = 0;

	while (!STAILQ_EMPTY(ids) && nskip != nqueued) {
		int color;

		if (cancel_cb) {
			err = cancel_cb(cancel_arg);
			if (err)
				break;
		}

		qid = STAILQ_FIRST(ids);
		STAILQ_REMOVE_HEAD(ids, entry);
		nqueued--;
		color = *((int *)qid->data);
		if (color == COLOR_SKIP)
			nskip--;

		if (got_object_idset_contains(skip, &qid->id)) {
			got_object_qid_free(qid);
			continue;
		}

		switch (color) {
		case COLOR_KEEP:
			if (got_object_idset_contains(keep, &qid->id)) {
				got_object_qid_free(qid);
				continue;
			}
			if (got_object_idset_contains(drop, &qid->id)) {
				err = paint_commit(qid, COLOR_SKIP);
				if (err)
					goto done;
			} else
				(*ncolored)++;
			err = got_object_idset_add(keep, &qid->id, NULL);
			if (err)
				goto done;
			break;
		case COLOR_DROP:
			if (got_object_idset_contains(drop, &qid->id)) {
				got_object_qid_free(qid);
				continue;
			}
			if (got_object_idset_contains(keep, &qid->id)) {
				err = paint_commit(qid, COLOR_SKIP);
				if (err)
					goto done;
			} else
				(*ncolored)++;
			err = got_object_idset_add(drop, &qid->id, NULL);
			if (err)
				goto done;
			break;
		case COLOR_SKIP:
			if (!got_object_idset_contains(skip, &qid->id)) {
				err = got_object_idset_add(skip, &qid->id,
				    NULL);
				if (err)
					goto done;
			}
			break;
		default:
			/* should not happen */
			err = got_error_fmt(GOT_ERR_NOT_IMPL,
			    "%s invalid commit color %d", __func__, color);
			goto done;
		}

		err = report_progress(progress_cb, progress_arg, rl,
		    *ncolored, 0, 0, 0L, 0, 0, 0, 0);
		if (err)
			break;


		err = got_object_open_as_commit(&commit, repo, &qid->id);
		if (err)
			break;

		parents = got_object_commit_get_parent_ids(commit);
		if (parents) {
			struct got_object_qid *pid;
			color = *((int *)qid->data);
			STAILQ_FOREACH(pid, parents, entry) {
				err = queue_commit_id(ids, &pid->id, color,
				    repo);
				if (err)
					break;
				nqueued++;
				if (color == COLOR_SKIP)
					nskip++;
			}
		}

		got_object_commit_close(commit);
		commit = NULL;
		got_object_qid_free(qid);
	}
done:
	if (commit)
		got_object_commit_close(commit);
	return err;
}

static const struct got_error *
findtwixt(struct got_object_id ***res, int *nres, int *ncolored,
    struct got_object_id **head, int nhead,
    struct got_object_id **tail, int ntail,
    struct got_repository *repo,
    got_pack_progress_cb progress_cb, void *progress_arg,
    struct got_ratelimit *rl, got_cancel_cb cancel_cb, void *cancel_arg)
{
	const struct got_error *err = NULL;
	struct got_object_id_queue ids;
	struct got_object_idset *keep, *drop, *skip = NULL;
	int i, nkeep;

	STAILQ_INIT(&ids);
	*res = NULL;
	*nres = 0;
	*ncolored = 0;

	keep = got_object_idset_alloc();
	if (keep == NULL)
		return got_error_from_errno("got_object_idset_alloc");

	drop = got_object_idset_alloc();
	if (drop == NULL) {
		err = got_error_from_errno("got_object_idset_alloc");
		goto done;
	}

	skip = got_object_idset_alloc();
	if (skip == NULL) {
		err = got_error_from_errno("got_object_idset_alloc");
		goto done;
	}

	for (i = 0; i < nhead; i++) {
		struct got_object_id *id = head[i];
		if (id == NULL)
			continue;
		err = queue_commit_or_tag_id(id, COLOR_KEEP, &ids, repo);
		if (err)
			goto done;
	}		

	for (i = 0; i < ntail; i++) {
		struct got_object_id *id = tail[i];
		if (id == NULL)
			continue;
		err = queue_commit_or_tag_id(id, COLOR_DROP, &ids, repo);
		if (err)
			goto done;
	}

	err = paint_commits(ncolored, &ids, nhead + ntail,
	    keep, drop, skip, repo, progress_cb, progress_arg, rl,
	    cancel_cb, cancel_arg);
	if (err)
		goto done;

	nkeep = got_object_idset_num_elements(keep);
	if (nkeep > 0) {
		struct append_id_arg arg;
		arg.array = calloc(nkeep, sizeof(struct got_object_id *));
		if (arg.array == NULL) {
			err = got_error_from_errno("calloc");
			goto done;
		}
		arg.idx = -1;
		arg.skip = skip;
		arg.drop = drop;
		err = got_object_idset_for_each(keep, append_id, &arg);
		if (err) {
			free(arg.array);
			goto done;
		}
		*res = arg.array;
		*nres = arg.idx + 1;
	}
done:
	got_object_idset_free(keep);
	got_object_idset_free(drop);
	if (skip)
		got_object_idset_free(skip);
	got_object_id_queue_free(&ids);
	return err;
}

static const struct got_error *
load_object_ids(int *ncolored, int *nfound, int *ntrees,
    struct got_object_idset *idset, struct got_object_id **theirs, int ntheirs,
    struct got_object_id **ours, int nours, struct got_repository *repo,
    uint32_t seed, int loose_obj_only, got_pack_progress_cb progress_cb,
    void *progress_arg, struct got_ratelimit *rl, got_cancel_cb cancel_cb,
    void *cancel_arg)
{
	const struct got_error *err = NULL;
	struct got_object_id **ids = NULL;
	int i, nobj = 0, obj_type;
	struct got_object_idset *idset_exclude;

	idset_exclude = got_object_idset_alloc();
	if (idset_exclude == NULL)
		return got_error_from_errno("got_object_idset_alloc");

	*ncolored = 0;
	*nfound = 0;
	*ntrees = 0;

	err = findtwixt(&ids, &nobj, ncolored, ours, nours, theirs, ntheirs,
	    repo, progress_cb, progress_arg, rl, cancel_cb, cancel_arg);
	if (err)
		goto done;

	for (i = 0; i < ntheirs; i++) {
		struct got_object_id *id = theirs[i];
		if (id == NULL)
			continue;
		err = got_object_get_type(&obj_type, repo, id);
		if (err)
			return err;
		if (obj_type == GOT_OBJ_TYPE_COMMIT) {
			err = load_commit(0, idset, idset_exclude, id, repo,
			    seed, loose_obj_only, ncolored, nfound, ntrees,
			    progress_cb, progress_arg, rl,
			    cancel_cb, cancel_arg);
			if (err)
				goto done;
		} else if (obj_type == GOT_OBJ_TYPE_TAG) {
			err = load_tag(0, idset, idset_exclude, id, repo,
			    seed, loose_obj_only, ncolored, nfound, ntrees,
			    progress_cb, progress_arg, rl,
			    cancel_cb, cancel_arg);
			if (err)
				goto done;
		}
	}

	for (i = 0; i < nobj; i++) {
		err = load_commit(1, idset, idset_exclude, ids[i], repo,
		    seed, loose_obj_only, ncolored, nfound, ntrees,
		    progress_cb, progress_arg, rl, cancel_cb, cancel_arg);
		if (err)
			goto done;
	}

	for (i = 0; i < nours; i++) {
		struct got_object_id *id = ours[i];
		struct got_pack_meta *m;
		if (id == NULL)
			continue;
		m = got_object_idset_get(idset, id);
		if (m == NULL) {
			err = got_object_get_type(&obj_type, repo, id);
			if (err)
				goto done;
		} else
			obj_type = m->obj_type;
		if (obj_type != GOT_OBJ_TYPE_TAG)
			continue;
		err = load_tag(1, idset, idset_exclude, id, repo,
		    seed, loose_obj_only, ncolored, nfound, ntrees,
		    progress_cb, progress_arg, rl, cancel_cb, cancel_arg);
		if (err)
			goto done;
	}
done:
	for (i = 0; i < nobj; i++) {
		free(ids[i]);
	}
	free(ids);
	got_object_idset_free(idset_exclude);
	return err;
}

const struct got_error *
hwrite(FILE *f, void *buf, off_t len, SHA1_CTX *ctx)
{
	size_t n;

	SHA1Update(ctx, buf, len);
	n = fwrite(buf, 1, len, f);
	if (n != len)
		return got_ferror(f, GOT_ERR_IO);
	return NULL;
}

const struct got_error *
hcopy(FILE *fsrc, FILE *fdst, off_t len, SHA1_CTX *ctx)
{
	unsigned char buf[65536];
	off_t remain = len;
	size_t n;

	while (remain > 0) {
		size_t copylen = MIN(sizeof(buf), remain);
		n = fread(buf, 1, copylen, fsrc);
		if (n != copylen)
			return got_ferror(fsrc, GOT_ERR_IO);
		SHA1Update(ctx, buf, copylen);
		n = fwrite(buf, 1, copylen, fdst);
		if (n != copylen)
			return got_ferror(fdst, GOT_ERR_IO);
		remain -= copylen;
	}

	return NULL;
}

const struct got_error *
hcopy_mmap(uint8_t *src, off_t src_offset, size_t src_size,
    FILE *fdst, off_t len, SHA1_CTX *ctx)
{
	size_t n;

	if (src_offset + len > src_size)
		return got_error(GOT_ERR_RANGE);

	SHA1Update(ctx, src + src_offset, len);
	n = fwrite(src + src_offset, 1, len, fdst);
	if (n != len)
		return got_ferror(fdst, GOT_ERR_IO);

	return NULL;
}

static void
putbe32(char *b, uint32_t n)
{
	b[0] = n >> 24;
	b[1] = n >> 16;
	b[2] = n >> 8;
	b[3] = n >> 0;
}

static int
write_order_cmp(const void *pa, const void *pb)
{
	struct got_pack_meta *a, *b, *ahd, *bhd;

	a = *(struct got_pack_meta **)pa;
	b = *(struct got_pack_meta **)pb;
	ahd = (a->head == NULL) ? a : a->head;
	bhd = (b->head == NULL) ? b : b->head;
	if (bhd->mtime < ahd->mtime)
		return -1;
	if (bhd->mtime > ahd->mtime)
		return 1;
	if (bhd < ahd)
		return -1;
	if (bhd > ahd)
		return 1;
	if (a->nchain != b->nchain)
		return a->nchain - b->nchain;
	if (a->mtime < b->mtime)
		return -1;
	if (a->mtime > b->mtime)
		return 1;
	return got_object_id_cmp(&a->id, &b->id);
}

static int
reuse_write_order_cmp(const void *pa, const void *pb)
{
	struct got_pack_meta *a, *b;

	a = *(struct got_pack_meta **)pa;
	b = *(struct got_pack_meta **)pb;

	if (a->reused_delta_offset < b->reused_delta_offset)
		return -1;
	if (a->reused_delta_offset > b->reused_delta_offset)
		return 1;
	return 0;
}

static const struct got_error *
packhdr(int *hdrlen, char *hdr, size_t bufsize, int obj_type, size_t len)
{
	size_t i;

	*hdrlen = 0;

	hdr[0] = obj_type << 4;
	hdr[0] |= len & 0xf;
	len >>= 4;
	for (i = 1; len != 0; i++){
		if (i >= bufsize)
			return got_error(GOT_ERR_NO_SPACE);
		hdr[i - 1] |= GOT_DELTA_SIZE_MORE;
		hdr[i] = len & GOT_DELTA_SIZE_VAL_MASK;
		len >>= GOT_DELTA_SIZE_SHIFT;
	}

	*hdrlen = i;
	return NULL;
}

static int
packoff(char *hdr, off_t off)
{
	int i, j;
	char rbuf[8];

	rbuf[0] = off & GOT_DELTA_SIZE_VAL_MASK;
	for (i = 1; (off >>= GOT_DELTA_SIZE_SHIFT) != 0; i++) {
		rbuf[i] = (--off & GOT_DELTA_SIZE_VAL_MASK) |
		    GOT_DELTA_SIZE_MORE;
	}

	j = 0;
	while (i > 0)
		hdr[j++] = rbuf[--i];
	return j;
}

static const struct got_error *
deltahdr(off_t *packfile_size, SHA1_CTX *ctx, FILE *packfile,
    struct got_pack_meta *m)
{
	const struct got_error *err;
	char buf[32];
	int nh;

	if (m->prev->off != 0) {
		err = packhdr(&nh, buf, sizeof(buf),
		    GOT_OBJ_TYPE_OFFSET_DELTA, m->delta_len);
		if (err)
			return err;
		nh += packoff(buf + nh, m->off - m->prev->off);
		err = hwrite(packfile, buf, nh, ctx);
		if (err)
			return err;
		*packfile_size += nh;
	} else {
		err = packhdr(&nh, buf, sizeof(buf),
		    GOT_OBJ_TYPE_REF_DELTA, m->delta_len);
		if (err)
			return err;
		err = hwrite(packfile, buf, nh, ctx);
		if (err)
			return err;
		*packfile_size += nh;
		err = hwrite(packfile, m->prev->id.sha1,
		    sizeof(m->prev->id.sha1), ctx);
		if (err)
			return err;
		*packfile_size += sizeof(m->prev->id.sha1);
	}

	return NULL;
}

static const struct got_error *
write_packed_object(off_t *packfile_size, FILE *packfile,
    FILE *delta_cache, uint8_t *delta_cache_map, size_t delta_cache_size,
    struct got_pack_meta *m, int *outfd, SHA1_CTX *ctx,
    struct got_repository *repo)
{
	const struct got_error *err = NULL;
	struct got_deflate_checksum csum;
	char buf[32];
	int nh;
	struct got_raw_object *raw = NULL;
	off_t outlen;

	csum.output_sha1 = ctx;
	csum.output_crc = NULL;

	m->off = ftello(packfile);
	if (m->delta_len == 0) {
		err = got_object_raw_open(&raw, outfd, repo, &m->id);
		if (err)
			goto done;
		err = packhdr(&nh, buf, sizeof(buf),
		    m->obj_type, raw->size);
		if (err)
			goto done;
		err = hwrite(packfile, buf, nh, ctx);
		if (err)
			goto done;
		*packfile_size += nh;
		if (raw->f == NULL) {
			err = got_deflate_to_file_mmap(&outlen,
			    raw->data + raw->hdrlen, 0, raw->size,
			    packfile, &csum);
			if (err)
				goto done;
		} else {
			if (fseeko(raw->f, raw->hdrlen, SEEK_SET)
			    == -1) {
				err = got_error_from_errno("fseeko");
				goto done;
			}
			err = got_deflate_to_file(&outlen, raw->f,
			    raw->size, packfile, &csum);
			if (err)
				goto done;
		}
		*packfile_size += outlen;
		got_object_raw_close(raw);
		raw = NULL;
	} else if (m->delta_buf) {
		err = deltahdr(packfile_size, ctx, packfile, m);
		if (err)
			goto done;
		err = hwrite(packfile, m->delta_buf,
		    m->delta_compressed_len, ctx);
		if (err)
			goto done;
		*packfile_size += m->delta_compressed_len;
		free(m->delta_buf);
		m->delta_buf = NULL;
	} else if (delta_cache_map) {
		err = deltahdr(packfile_size, ctx, packfile, m);
		if (err)
			goto done;
		err = hcopy_mmap(delta_cache_map, m->delta_offset,
		    delta_cache_size, packfile, m->delta_compressed_len,
		    ctx);
		if (err)
			goto done;
		*packfile_size += m->delta_compressed_len;
	} else {
		if (fseeko(delta_cache, m->delta_offset, SEEK_SET)
		    == -1) {
			err = got_error_from_errno("fseeko");
			goto done;
		}
		err = deltahdr(packfile_size, ctx, packfile, m);
		if (err)
			goto done;
		err = hcopy(delta_cache, packfile,
		    m->delta_compressed_len, ctx);
		if (err)
			goto done;
		*packfile_size += m->delta_compressed_len;
	}
done:
	if (raw)
		got_object_raw_close(raw);
	return err;
}

static const struct got_error *
genpack(uint8_t *pack_sha1, FILE *packfile, FILE *delta_cache,
    struct got_pack_meta **deltify, int ndeltify,
    struct got_pack_meta **reuse, int nreuse,
    int ncolored, int nfound, int ntrees, int nours,
    struct got_repository *repo,
    got_pack_progress_cb progress_cb, void *progress_arg,
    struct got_ratelimit *rl,
    got_cancel_cb cancel_cb, void *cancel_arg)
{
	const struct got_error *err = NULL;
	int i;
	SHA1_CTX ctx;
	struct got_pack_meta *m;
	char buf[32];
	size_t n;
	off_t packfile_size = 0;
	int outfd = -1;
	int delta_cache_fd = -1;
	uint8_t *delta_cache_map = NULL;
	size_t delta_cache_size = 0;

	SHA1Init(&ctx);

#ifndef GOT_PACK_NO_MMAP
	delta_cache_fd = dup(fileno(delta_cache));
	if (delta_cache_fd != -1) {
		struct stat sb;
		if (fstat(delta_cache_fd, &sb) == -1) {
			err = got_error_from_errno("fstat");
			goto done;
		}
		if (sb.st_size > 0 && sb.st_size <= SIZE_MAX) {
			delta_cache_map = mmap(NULL, sb.st_size,
			    PROT_READ, MAP_PRIVATE, delta_cache_fd, 0);
			if (delta_cache_map == MAP_FAILED) {
				if (errno != ENOMEM) {
					err = got_error_from_errno("mmap");
					goto done;
				}
				delta_cache_map = NULL; /* fallback on stdio */
			} else
				delta_cache_size = (size_t)sb.st_size;
		}
	}
#endif
	err = hwrite(packfile, "PACK", 4, &ctx);
	if (err)
		goto done;
	putbe32(buf, GOT_PACKFILE_VERSION);
	err = hwrite(packfile, buf, 4, &ctx);
	if (err)
		goto done;
	putbe32(buf, ndeltify + nreuse);
	err = hwrite(packfile, buf, 4, &ctx);
	if (err)
		goto done;

	qsort(deltify, ndeltify, sizeof(struct got_pack_meta *),
	    write_order_cmp);
	for (i = 0; i < ndeltify; i++) {
		err = report_progress(progress_cb, progress_arg, rl,
		    ncolored, nfound, ntrees, packfile_size, nours,
		    ndeltify + nreuse, ndeltify + nreuse, i);
		if (err)
			goto done;
		m = deltify[i];
		err = write_packed_object(&packfile_size, packfile,
		    delta_cache, delta_cache_map, delta_cache_size,
		    m, &outfd, &ctx, repo);
		if (err)
			goto done;
	}

	qsort(reuse, nreuse, sizeof(struct got_pack_meta *),
	    reuse_write_order_cmp);
	for (i = 0; i < nreuse; i++) {
		err = report_progress(progress_cb, progress_arg, rl,
		    ncolored, nfound, ntrees, packfile_size, nours,
		    ndeltify + nreuse, ndeltify + nreuse, ndeltify + i);
		if (err)
			goto done;
		m = reuse[i];
		err = write_packed_object(&packfile_size, packfile,
		    delta_cache, delta_cache_map, delta_cache_size,
		    m, &outfd, &ctx, repo);
		if (err)
			goto done;
	}

	SHA1Final(pack_sha1, &ctx);
	n = fwrite(pack_sha1, 1, SHA1_DIGEST_LENGTH, packfile);
	if (n != SHA1_DIGEST_LENGTH)
		err = got_ferror(packfile, GOT_ERR_IO);
	packfile_size += SHA1_DIGEST_LENGTH;
	packfile_size += sizeof(struct got_packfile_hdr);
	if (progress_cb) {
		err = progress_cb(progress_arg, ncolored, nfound, ntrees,
		    packfile_size, nours, ndeltify + nreuse,
		    ndeltify + nreuse, ndeltify + nreuse);
		if (err)
			goto done;
	}
done:
	if (outfd != -1 && close(outfd) == -1 && err == NULL)
		err = got_error_from_errno("close");
	if (delta_cache_map && munmap(delta_cache_map, delta_cache_size) == -1)
		err = got_error_from_errno("munmap");
	if (delta_cache_fd != -1 && close(delta_cache_fd) == -1 && err == NULL)
		err = got_error_from_errno("close");
	return err;
}

static const struct got_error *
add_meta_idset_cb(struct got_object_id *id, void *data, void *arg)
{
	struct got_pack_meta *m = data;
	struct got_pack_metavec *v = arg;

	if (m->reused_delta_offset != 0)
		return NULL;

	return add_meta(m, v);
}

const struct got_error *
got_pack_create(uint8_t *packsha1, FILE *packfile,
    struct got_object_id **theirs, int ntheirs,
    struct got_object_id **ours, int nours,
    struct got_repository *repo, int loose_obj_only, int allow_empty,
    got_pack_progress_cb progress_cb, void *progress_arg,
    got_cancel_cb cancel_cb, void *cancel_arg)
{
	const struct got_error *err;
	int delta_cache_fd = -1;
	FILE *delta_cache = NULL;
	struct got_object_idset *idset;
	struct got_ratelimit rl;
	struct got_pack_metavec deltify, reuse;
	int ncolored = 0, nfound = 0, ntrees = 0;
	size_t ndeltify;
	uint32_t seed;

	seed = arc4random();

	memset(&deltify, 0, sizeof(deltify));
	memset(&reuse, 0, sizeof(reuse));

	got_ratelimit_init(&rl, 0, 500);

	idset = got_object_idset_alloc();
	if (idset == NULL)
		return got_error_from_errno("got_object_idset_alloc");

	err = load_object_ids(&ncolored, &nfound, &ntrees, idset, theirs,
	    ntheirs, ours, nours, repo, seed, loose_obj_only,
	    progress_cb, progress_arg, &rl, cancel_cb, cancel_arg);
	if (err)
		goto done;

	if (progress_cb) {
		err = progress_cb(progress_arg, ncolored, nfound, ntrees,
		    0L, nours, got_object_idset_num_elements(idset), 0, 0);
		if (err)
			goto done;
	}

	if (got_object_idset_num_elements(idset) == 0 && !allow_empty) {
		err = got_error(GOT_ERR_CANNOT_PACK);
		goto done;
	}

	delta_cache_fd = got_opentempfd();
	if (delta_cache_fd == -1) {
		err = got_error_from_errno("got_opentemp");
		goto done;
	}

	reuse.metasz = 64;
	reuse.meta = calloc(reuse.metasz,
	    sizeof(struct got_pack_meta *));
	if (reuse.meta == NULL) {
		err = got_error_from_errno("calloc");
		goto done;
	}

	err = search_deltas(&reuse, idset, delta_cache_fd, ncolored, nfound,
	    ntrees, nours, repo, progress_cb, progress_arg, &rl,
	    cancel_cb, cancel_arg);
	if (err)
		goto done;

	delta_cache = fdopen(delta_cache_fd, "a+");
	if (delta_cache == NULL) {
		err = got_error_from_errno("fdopen");
		goto done;
	}
	delta_cache_fd = -1;

	if (fseeko(delta_cache, 0L, SEEK_END) == -1) {
		err = got_error_from_errno("fseeko");
		goto done;
	}

	ndeltify = got_object_idset_num_elements(idset) - reuse.nmeta;
	if (ndeltify > 0) {
		deltify.meta = calloc(ndeltify, sizeof(struct got_pack_meta *));
		if (deltify.meta == NULL) {
			err = got_error_from_errno("calloc");
			goto done;
		}
		deltify.metasz = ndeltify;

		err = got_object_idset_for_each(idset, add_meta_idset_cb,
		    &deltify);
		if (err)
			goto done;
		if (deltify.nmeta > 0) {
			err = pick_deltas(deltify.meta, deltify.nmeta,
			    ncolored, nfound, ntrees, nours, reuse.nmeta,
			    delta_cache, repo, progress_cb, progress_arg, &rl,
			    cancel_cb, cancel_arg);
			if (err)
				goto done;
		}
	}

	if (fflush(delta_cache) == EOF) {
		err = got_error_from_errno("fflush");
		goto done;
	}
	err = genpack(packsha1, packfile, delta_cache, deltify.meta,
	    deltify.nmeta, reuse.meta, reuse.nmeta, ncolored, nfound, ntrees,
	    nours, repo, progress_cb, progress_arg, &rl,
	    cancel_cb, cancel_arg);
	if (err)
		goto done;
done:
	free_nmeta(deltify.meta, deltify.nmeta);
	free_nmeta(reuse.meta, reuse.nmeta);
	got_object_idset_free(idset);
	if (delta_cache_fd != -1 && close(delta_cache_fd) == -1 && err == NULL)
		err = got_error_from_errno("close");
	if (delta_cache && fclose(delta_cache) == EOF && err == NULL)
		err = got_error_from_errno("fclose");
	return err;
}
