/*
 * Copyright (c) 2017 Stefan Sperling <stsp@openbsd.org>
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

#include <sys/queue.h>
#include <sys/stat.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sha1.h>
#include <zlib.h>

#include "got_object.h"
#include "got_repository.h"
#include "got_error.h"
#include "got_diff.h"
#include "got_path.h"
#include "got_cancel.h"
#include "got_worktree.h"
#include "got_opentemp.h"

#include "got_lib_diff.h"
#include "got_lib_delta.h"
#include "got_lib_inflate.h"
#include "got_lib_object.h"

static const struct got_error *
add_line_offset(off_t **line_offsets, size_t *nlines, off_t off)
{
	off_t *p;

	p = reallocarray(*line_offsets, *nlines + 1, sizeof(off_t));
	if (p == NULL)
		return got_error_from_errno("reallocarray");
	*line_offsets = p;
	(*line_offsets)[*nlines] = off;
	(*nlines)++;
	return NULL;
}

static const struct got_error *
diff_blobs(off_t **line_offsets, size_t *nlines,
    struct got_diffreg_result **resultp, struct got_blob_object *blob1,
    struct got_blob_object *blob2, FILE *f1, FILE *f2,
    const char *label1, const char *label2, mode_t mode1, mode_t mode2,
    int diff_context, int ignore_whitespace, int force_text_diff, FILE *outfile)
{
	const struct got_error *err = NULL, *free_err;
	char hex1[SHA1_DIGEST_STRING_LENGTH];
	char hex2[SHA1_DIGEST_STRING_LENGTH];
	const char *idstr1 = NULL, *idstr2 = NULL;
	off_t size1, size2;
	struct got_diffreg_result *result;
	off_t outoff = 0;
	int n;

	if (line_offsets && *line_offsets && *nlines > 0)
		outoff = (*line_offsets)[*nlines - 1];
	else if (line_offsets) {
		err = add_line_offset(line_offsets, nlines, 0);
		if (err)
			goto done;
	}

	if (resultp)
		*resultp = NULL;

	if (f1) {
		err = got_opentemp_truncate(f1);
		if (err)
			goto done;
	}
	if (f2) {
		err = got_opentemp_truncate(f2);
		if (err)
			goto done;
	}

	size1 = 0;
	if (blob1) {
		idstr1 = got_object_blob_id_str(blob1, hex1, sizeof(hex1));
		err = got_object_blob_dump_to_file(&size1, NULL, NULL, f1,
		    blob1);
		if (err)
			goto done;
	} else
		idstr1 = "/dev/null";

	size2 = 0;
	if (blob2) {
		idstr2 = got_object_blob_id_str(blob2, hex2, sizeof(hex2));
		err = got_object_blob_dump_to_file(&size2, NULL, NULL, f2,
		    blob2);
		if (err)
			goto done;
	} else
		idstr2 = "/dev/null";

	if (outfile) {
		char *modestr1 = NULL, *modestr2 = NULL;
		int modebits;
		if (mode1 && mode1 != mode2) {
			if (S_ISLNK(mode1))
				modebits = S_IFLNK;
			else
				modebits = (S_IRWXU | S_IRWXG | S_IRWXO);
			if (asprintf(&modestr1, " (mode %o)",
			    mode1 & modebits) == -1) {
				err = got_error_from_errno("asprintf");
				goto done;
			}
		}
		if (mode2 && mode1 != mode2) {
			if (S_ISLNK(mode2))
				modebits = S_IFLNK;
			else
				modebits = (S_IRWXU | S_IRWXG | S_IRWXO);
			if (asprintf(&modestr2, " (mode %o)",
			    mode2 & modebits) == -1) {
				err = got_error_from_errno("asprintf");
				goto done;
			}
		}
		n = fprintf(outfile, "blob - %s%s\n", idstr1,
		    modestr1 ? modestr1 : "");
		if (n < 0)
			goto done;
		outoff += n;
		if (line_offsets) {
			err = add_line_offset(line_offsets, nlines, outoff);
			if (err)
				goto done;
		}

		n = fprintf(outfile, "blob + %s%s\n", idstr2,
		    modestr2 ? modestr2 : "");
		if (n < 0)
			goto done;
		outoff += n;
		if (line_offsets) {
			err = add_line_offset(line_offsets, nlines, outoff);
			if (err)
				goto done;
		}

		free(modestr1);
		free(modestr2);
	}
	err = got_diffreg(&result, f1, f2, GOT_DIFF_ALGORITHM_PATIENCE,
	    ignore_whitespace, force_text_diff);
	if (err)
		goto done;

	if (outfile) {
		err = got_diffreg_output(line_offsets, nlines, result,
		    blob1 != NULL, blob2 != NULL,
		    label1 ? label1 : idstr1,
		    label2 ? label2 : idstr2,
		    GOT_DIFF_OUTPUT_UNIDIFF, diff_context, outfile);
		if (err)
			goto done;
	}

	if (resultp && err == NULL)
		*resultp = result;
	else {
		free_err = got_diffreg_result_free(result);
		if (free_err && err == NULL)
			err = free_err;
	}
done:
	return err;
}

const struct got_error *
got_diff_blob_output_unidiff(void *arg, struct got_blob_object *blob1,
    struct got_blob_object *blob2, FILE *f1, FILE *f2,
    struct got_object_id *id1, struct got_object_id *id2,
    const char *label1, const char *label2, mode_t mode1, mode_t mode2,
    struct got_repository *repo)
{
	struct got_diff_blob_output_unidiff_arg *a = arg;

	return diff_blobs(&a->line_offsets, &a->nlines, NULL,
	    blob1, blob2, f1, f2, label1, label2, mode1, mode2, a->diff_context,
	    a->ignore_whitespace, a->force_text_diff, a->outfile);
}

const struct got_error *
got_diff_blob(off_t **line_offsets, size_t *nlines,
    struct got_blob_object *blob1, struct got_blob_object *blob2,
    FILE *f1, FILE *f2, const char *label1, const char *label2,
    int diff_context, int ignore_whitespace, int force_text_diff,
    FILE *outfile)
{
	return diff_blobs(line_offsets, nlines, NULL, blob1, blob2, f1, f2,
	    label1, label2, 0, 0, diff_context, ignore_whitespace,
	    force_text_diff, outfile);
}

static const struct got_error *
diff_blob_file(struct got_diffreg_result **resultp,
    struct got_blob_object *blob1, FILE *f1, off_t size1, const char *label1,
    FILE *f2, size_t size2, const char *label2, int diff_context,
    int ignore_whitespace, int force_text_diff, FILE *outfile)
{
	const struct got_error *err = NULL, *free_err;
	char hex1[SHA1_DIGEST_STRING_LENGTH];
	const char *idstr1 = NULL;
	struct got_diffreg_result *result = NULL;

	if (resultp)
		*resultp = NULL;

	if (blob1)
		idstr1 = got_object_blob_id_str(blob1, hex1, sizeof(hex1));
	else
		idstr1 = "/dev/null";

	if (outfile) {
		fprintf(outfile, "blob - %s\n", label1 ? label1 : idstr1);
		fprintf(outfile, "file + %s\n",
		    f2 == NULL ? "/dev/null" : label2);
	}

	err = got_diffreg(&result, f1, f2, GOT_DIFF_ALGORITHM_PATIENCE, 
	    ignore_whitespace, force_text_diff);
	if (err)
		goto done;

	if (outfile) {
		err = got_diffreg_output(NULL, NULL, result,
		    f1 != NULL, f2 != NULL,
		    label2, /* show local file's path, not a blob ID */
		    label2, GOT_DIFF_OUTPUT_UNIDIFF,
		    diff_context, outfile);
		if (err)
			goto done;
	}

	if (resultp && err == NULL)
		*resultp = result;
	else if (result) {
		free_err = got_diffreg_result_free(result);
		if (free_err && err == NULL)
			err = free_err;
	}
done:
	return err;
}

const struct got_error *
got_diff_blob_file(struct got_blob_object *blob1, FILE *f1, off_t size1,
    const char *label1, FILE *f2, size_t size2, const char *label2,
    int diff_context, int ignore_whitespace, int force_text_diff, FILE *outfile)
{
	return diff_blob_file(NULL, blob1, f1, size1, label1, f2, size2, label2,
	    diff_context, ignore_whitespace, force_text_diff, outfile);
}

static const struct got_error *
diff_added_blob(struct got_object_id *id, FILE *f, const char *label,
    mode_t mode, struct got_repository *repo,
    got_diff_blob_cb cb, void *cb_arg)
{
	const struct got_error *err;
	struct got_blob_object  *blob = NULL;
	struct got_object *obj = NULL;

	err = got_object_open(&obj, repo, id);
	if (err)
		return err;

	err = got_object_blob_open(&blob, repo, obj, 8192);
	if (err)
		goto done;
	err = cb(cb_arg, NULL, blob, NULL, f, NULL, id,
	    NULL, label, 0, mode, repo);
done:
	got_object_close(obj);
	if (blob)
		got_object_blob_close(blob);
	return err;
}

static const struct got_error *
diff_modified_blob(struct got_object_id *id1, struct got_object_id *id2,
    FILE *f1, FILE *f2, const char *label1, const char *label2,
    mode_t mode1, mode_t mode2, struct got_repository *repo,
    got_diff_blob_cb cb, void *cb_arg)
{
	const struct got_error *err;
	struct got_object *obj1 = NULL;
	struct got_object *obj2 = NULL;
	struct got_blob_object *blob1 = NULL;
	struct got_blob_object *blob2 = NULL;

	err = got_object_open(&obj1, repo, id1);
	if (err)
		return err;
	if (obj1->type != GOT_OBJ_TYPE_BLOB) {
		err = got_error(GOT_ERR_OBJ_TYPE);
		goto done;
	}

	err = got_object_open(&obj2, repo, id2);
	if (err)
		goto done;
	if (obj2->type != GOT_OBJ_TYPE_BLOB) {
		err = got_error(GOT_ERR_BAD_OBJ_DATA);
		goto done;
	}

	err = got_object_blob_open(&blob1, repo, obj1, 8192);
	if (err)
		goto done;

	err = got_object_blob_open(&blob2, repo, obj2, 8192);
	if (err)
		goto done;

	err = cb(cb_arg, blob1, blob2, f1, f2, id1, id2, label1, label2,
	    mode1, mode2, repo);
done:
	if (obj1)
		got_object_close(obj1);
	if (obj2)
		got_object_close(obj2);
	if (blob1)
		got_object_blob_close(blob1);
	if (blob2)
		got_object_blob_close(blob2);
	return err;
}

static const struct got_error *
diff_deleted_blob(struct got_object_id *id, FILE *f, const char *label,
    mode_t mode, struct got_repository *repo, got_diff_blob_cb cb, void *cb_arg)
{
	const struct got_error *err;
	struct got_blob_object  *blob = NULL;
	struct got_object *obj = NULL;

	err = got_object_open(&obj, repo, id);
	if (err)
		return err;

	err = got_object_blob_open(&blob, repo, obj, 8192);
	if (err)
		goto done;
	err = cb(cb_arg, blob, NULL, f, NULL, id, NULL, label, NULL,
	    mode, 0, repo);
done:
	got_object_close(obj);
	if (blob)
		got_object_blob_close(blob);
	return err;
}

static const struct got_error *
diff_added_tree(struct got_object_id *id, FILE *f, const char *label,
    struct got_repository *repo, got_diff_blob_cb cb, void *cb_arg,
    int diff_content)
{
	const struct got_error *err = NULL;
	struct got_object *treeobj = NULL;
	struct got_tree_object *tree = NULL;

	err = got_object_open(&treeobj, repo, id);
	if (err)
		goto done;

	if (treeobj->type != GOT_OBJ_TYPE_TREE) {
		err = got_error(GOT_ERR_OBJ_TYPE);
		goto done;
	}

	err = got_object_tree_open(&tree, repo, treeobj);
	if (err)
		goto done;

	err = got_diff_tree(NULL, tree, NULL, f, NULL, label,
	    repo, cb, cb_arg, diff_content);
done:
	if (tree)
		got_object_tree_close(tree);
	if (treeobj)
		got_object_close(treeobj);
	return err;
}

static const struct got_error *
diff_modified_tree(struct got_object_id *id1, struct got_object_id *id2,
    FILE *f1, FILE *f2, const char *label1, const char *label2,
    struct got_repository *repo, got_diff_blob_cb cb, void *cb_arg,
    int diff_content)
{
	const struct got_error *err;
	struct got_object *treeobj1 = NULL;
	struct got_object *treeobj2 = NULL;
	struct got_tree_object *tree1 = NULL;
	struct got_tree_object *tree2 = NULL;

	err = got_object_open(&treeobj1, repo, id1);
	if (err)
		goto done;

	if (treeobj1->type != GOT_OBJ_TYPE_TREE) {
		err = got_error(GOT_ERR_OBJ_TYPE);
		goto done;
	}

	err = got_object_open(&treeobj2, repo, id2);
	if (err)
		goto done;

	if (treeobj2->type != GOT_OBJ_TYPE_TREE) {
		err = got_error(GOT_ERR_OBJ_TYPE);
		goto done;
	}

	err = got_object_tree_open(&tree1, repo, treeobj1);
	if (err)
		goto done;

	err = got_object_tree_open(&tree2, repo, treeobj2);
	if (err)
		goto done;

	err = got_diff_tree(tree1, tree2, f1, f2, label1, label2,
	    repo, cb, cb_arg, diff_content);

done:
	if (tree1)
		got_object_tree_close(tree1);
	if (tree2)
		got_object_tree_close(tree2);
	if (treeobj1)
		got_object_close(treeobj1);
	if (treeobj2)
		got_object_close(treeobj2);
	return err;
}

static const struct got_error *
diff_deleted_tree(struct got_object_id *id, FILE *f, const char *label,
    struct got_repository *repo, got_diff_blob_cb cb, void *cb_arg,
    int diff_content)
{
	const struct got_error *err;
	struct got_object *treeobj = NULL;
	struct got_tree_object *tree = NULL;

	err = got_object_open(&treeobj, repo, id);
	if (err)
		goto done;

	if (treeobj->type != GOT_OBJ_TYPE_TREE) {
		err = got_error(GOT_ERR_OBJ_TYPE);
		goto done;
	}

	err = got_object_tree_open(&tree, repo, treeobj);
	if (err)
		goto done;

	err = got_diff_tree(tree, NULL, f, NULL, label, NULL,
	    repo, cb, cb_arg, diff_content);
done:
	if (tree)
		got_object_tree_close(tree);
	if (treeobj)
		got_object_close(treeobj);
	return err;
}

static const struct got_error *
diff_kind_mismatch(struct got_object_id *id1, struct got_object_id *id2,
    const char *label1, const char *label2, struct got_repository *repo,
    got_diff_blob_cb cb, void *cb_arg)
{
	/* XXX TODO */
	return NULL;
}

static const struct got_error *
diff_entry_old_new(struct got_tree_entry *te1, struct got_tree_entry *te2,
    FILE *f1, FILE *f2, const char *label1, const char *label2,
    struct got_repository *repo, got_diff_blob_cb cb, void *cb_arg,
    int diff_content)
{
	const struct got_error *err = NULL;
	int id_match;

	if (got_object_tree_entry_is_submodule(te1))
		return NULL;

	if (te2 == NULL) {
		if (S_ISDIR(te1->mode))
			err = diff_deleted_tree(&te1->id, f1, label1, repo,
			    cb, cb_arg, diff_content);
		else {
			if (diff_content)
				err = diff_deleted_blob(&te1->id, f1, label1,
				    te1->mode, repo, cb, cb_arg);
			else
				err = cb(cb_arg, NULL, NULL, NULL, NULL,
				    &te1->id, NULL, label1, NULL,
				    te1->mode, 0, repo);
		}
		return err;
	} else if (got_object_tree_entry_is_submodule(te2))
		return NULL;

	id_match = (got_object_id_cmp(&te1->id, &te2->id) == 0);
	if (S_ISDIR(te1->mode) && S_ISDIR(te2->mode)) {
		if (!id_match)
			return diff_modified_tree(&te1->id, &te2->id, f1, f2,
			    label1, label2, repo, cb, cb_arg, diff_content);
	} else if ((S_ISREG(te1->mode) || S_ISLNK(te1->mode)) &&
	    (S_ISREG(te2->mode) || S_ISLNK(te2->mode))) {
		if (!id_match ||
		    ((te1->mode & (S_IFLNK | S_IXUSR))) !=
		    (te2->mode & (S_IFLNK | S_IXUSR))) {
			if (diff_content)
				return diff_modified_blob(&te1->id, &te2->id,
				    f1, f2, label1, label2, te1->mode,
				    te2->mode, repo, cb, cb_arg);
			else
				return cb(cb_arg, NULL, NULL, NULL, NULL,
				    &te1->id, &te2->id, label1, label2,
				    te1->mode, te2->mode, repo);
		}
	}

	if (id_match)
		return NULL;

	return diff_kind_mismatch(&te1->id, &te2->id, label1, label2, repo,
	    cb, cb_arg);
}

static const struct got_error *
diff_entry_new_old(struct got_tree_entry *te2,
    struct got_tree_entry *te1, FILE *f2, const char *label2,
    struct got_repository *repo, got_diff_blob_cb cb, void *cb_arg,
    int diff_content)
{
	if (te1 != NULL) /* handled by diff_entry_old_new() */
		return NULL;

	if (got_object_tree_entry_is_submodule(te2))
		return NULL;

	if (S_ISDIR(te2->mode))
		return diff_added_tree(&te2->id, f2, label2,
		    repo, cb, cb_arg, diff_content);

	if (diff_content)
		return diff_added_blob(&te2->id, f2, label2, te2->mode,
		    repo, cb, cb_arg);

	return cb(cb_arg, NULL, NULL, NULL, NULL, NULL, &te2->id,
	    NULL, label2, 0, te2->mode, repo);
}

const struct got_error *
got_diff_tree_collect_changed_paths(void *arg, struct got_blob_object *blob1,
    struct got_blob_object *blob2, FILE *f1, FILE *f2,
    struct got_object_id *id1, struct got_object_id *id2,
    const char *label1, const char *label2,
    mode_t mode1, mode_t mode2, struct got_repository *repo)
{
	const struct got_error *err = NULL;
	struct got_pathlist_head *paths = arg;
	struct got_diff_changed_path *change = NULL;
	char *path = NULL;

	path = strdup(label2 ? label2 : label1);
	if (path == NULL)
		return got_error_from_errno("malloc");

	change = malloc(sizeof(*change));
	if (change == NULL) {
		err = got_error_from_errno("malloc");
		goto done;
	}

	change->status = GOT_STATUS_NO_CHANGE;
	if (id1 == NULL)
		change->status = GOT_STATUS_ADD;
	else if (id2 == NULL)
		change->status = GOT_STATUS_DELETE;
	else {
		if (got_object_id_cmp(id1, id2) != 0)
			change->status = GOT_STATUS_MODIFY;
		else if (mode1 != mode2)
			change->status = GOT_STATUS_MODE_CHANGE;
	}

	err = got_pathlist_append(paths, path, change);
done:
	if (err) {
		free(path);
		free(change);
	}
	return err;
}

const struct got_error *
got_diff_tree(struct got_tree_object *tree1, struct got_tree_object *tree2,
    FILE *f1, FILE *f2, const char *label1, const char *label2,
    struct got_repository *repo, got_diff_blob_cb cb, void *cb_arg,
    int diff_content)
{
	const struct got_error *err = NULL;
	struct got_tree_entry *te1 = NULL;
	struct got_tree_entry *te2 = NULL;
	char *l1 = NULL, *l2 = NULL;
	int tidx1 = 0, tidx2 = 0;

	if (tree1) {
		te1 = got_object_tree_get_entry(tree1, 0);
		if (te1 && asprintf(&l1, "%s%s%s", label1, label1[0] ? "/" : "",
		    te1->name) == -1)
			return got_error_from_errno("asprintf");
	}
	if (tree2) {
		te2 = got_object_tree_get_entry(tree2, 0);
		if (te2 && asprintf(&l2, "%s%s%s", label2, label2[0] ? "/" : "",
		    te2->name) == -1)
			return got_error_from_errno("asprintf");
	}

	do {
		if (te1) {
			struct got_tree_entry *te = NULL;
			if (tree2)
				te = got_object_tree_find_entry(tree2,
				    te1->name);
			if (te) {
				free(l2);
				l2 = NULL;
				if (te && asprintf(&l2, "%s%s%s", label2,
				    label2[0] ? "/" : "", te->name) == -1)
					return
					    got_error_from_errno("asprintf");
			}
			err = diff_entry_old_new(te1, te, f1, f2, l1, l2,
			    repo, cb, cb_arg, diff_content);
			if (err)
				break;
		}

		if (te2) {
			struct got_tree_entry *te = NULL;
			if (tree1)
				te = got_object_tree_find_entry(tree1,
				    te2->name);
			free(l2);
			if (te) {
				if (asprintf(&l2, "%s%s%s", label2,
				    label2[0] ? "/" : "", te->name) == -1)
					return
					    got_error_from_errno("asprintf");
			} else {
				if (asprintf(&l2, "%s%s%s", label2,
				    label2[0] ? "/" : "", te2->name) == -1)
					return
					    got_error_from_errno("asprintf");
			}
			err = diff_entry_new_old(te2, te, f2, l2, repo,
			    cb, cb_arg, diff_content);
			if (err)
				break;
		}

		free(l1);
		l1 = NULL;
		if (te1) {
			tidx1++;
			te1 = got_object_tree_get_entry(tree1, tidx1);
			if (te1 &&
			    asprintf(&l1, "%s%s%s", label1,
			    label1[0] ? "/" : "", te1->name) == -1)
				return got_error_from_errno("asprintf");
		}
		free(l2);
		l2 = NULL;
		if (te2) {
			tidx2++;
			te2 = got_object_tree_get_entry(tree2, tidx2);
			if (te2 &&
			    asprintf(&l2, "%s%s%s", label2,
			        label2[0] ? "/" : "", te2->name) == -1)
				return got_error_from_errno("asprintf");
		}
	} while (te1 || te2);

	return err;
}

const struct got_error *
got_diff_objects_as_blobs(off_t **line_offsets, size_t *nlines,
    FILE *f1, FILE *f2, struct got_object_id *id1, struct got_object_id *id2,
    const char *label1, const char *label2, int diff_context,
    int ignore_whitespace, int force_text_diff,
    struct got_repository *repo, FILE *outfile)
{
	const struct got_error *err;
	struct got_blob_object *blob1 = NULL, *blob2 = NULL;

	if (id1 == NULL && id2 == NULL)
		return got_error(GOT_ERR_NO_OBJ);

	if (id1) {
		err = got_object_open_as_blob(&blob1, repo, id1, 8192);
		if (err)
			goto done;
	}
	if (id2) {
		err = got_object_open_as_blob(&blob2, repo, id2, 8192);
		if (err)
			goto done;
	}
	err = got_diff_blob(line_offsets, nlines, blob1, blob2, f1, f2,
	    label1, label2, diff_context, ignore_whitespace, force_text_diff,
	    outfile);
done:
	if (blob1)
		got_object_blob_close(blob1);
	if (blob2)
		got_object_blob_close(blob2);
	return err;
}

static const struct got_error *
diff_paths(struct got_tree_object *tree1, struct got_tree_object *tree2,
    FILE *f1, FILE *f2, struct got_pathlist_head *paths,
    struct got_repository *repo, got_diff_blob_cb cb, void *cb_arg)
{
	const struct got_error *err = NULL;
	struct got_pathlist_entry *pe;
	struct got_object_id *id1 = NULL, *id2 = NULL;
	struct got_tree_object *subtree1 = NULL, *subtree2 = NULL;
	struct got_blob_object *blob1 = NULL, *blob2 = NULL;

	TAILQ_FOREACH(pe, paths, entry) {
		int type1 = GOT_OBJ_TYPE_ANY, type2 = GOT_OBJ_TYPE_ANY;
		mode_t mode1 = 0, mode2 = 0;

		free(id1);
		id1 = NULL;
		free(id2);
		id2 = NULL;
		if (subtree1) {
			got_object_tree_close(subtree1);
			subtree1 = NULL;
		}
		if (subtree2) {
			got_object_tree_close(subtree2);
			subtree2 = NULL;
		}
		if (blob1) {
			got_object_blob_close(blob1);
			blob1 = NULL;
		}
		if (blob2) {
			got_object_blob_close(blob2);
			blob2 = NULL;
		}

		err = got_object_tree_find_path(&id1, &mode1, repo, tree1,
		    pe->path);
		if (err && err->code != GOT_ERR_NO_TREE_ENTRY)
			goto done;
		err = got_object_tree_find_path(&id2, &mode2, repo, tree2,
		    pe->path);
		if (err && err->code != GOT_ERR_NO_TREE_ENTRY)
			goto done;
		if (id1 == NULL && id2 == NULL) {
			err = got_error_path(pe->path, GOT_ERR_NO_TREE_ENTRY);
			goto done;
		}
		if (id1) {
			err = got_object_get_type(&type1, repo, id1);
			if (err)
				goto done;
		}
		if (id2) {
			err = got_object_get_type(&type2, repo, id2);
			if (err)
				goto done;
		}
		if (type1 == GOT_OBJ_TYPE_ANY &&
		    type2 == GOT_OBJ_TYPE_ANY) {
			err = got_error_path(pe->path, GOT_ERR_NO_OBJ);
			goto done;
		} else if (type1 != GOT_OBJ_TYPE_ANY &&
		    type2 != GOT_OBJ_TYPE_ANY && type1 != type2) {
			err = got_error(GOT_ERR_OBJ_TYPE);
			goto done;
		}

		if (type1 == GOT_OBJ_TYPE_BLOB ||
		    type2 == GOT_OBJ_TYPE_BLOB) {
			if (id1) {
				err = got_object_open_as_blob(&blob1, repo,
				    id1, 8192);
				if (err)
					goto done;
			}
			if (id2) {
				err = got_object_open_as_blob(&blob2, repo,
				    id2, 8192);
				if (err)
					goto done;
			}
			err = cb(cb_arg, blob1, blob2, f1, f2, id1, id2,
			    id1 ? pe->path : "/dev/null",
			    id2 ? pe->path : "/dev/null",
			    mode1, mode2, repo);
			if (err)
				goto done;
		} else if (type1 == GOT_OBJ_TYPE_TREE ||
		    type2 == GOT_OBJ_TYPE_TREE) {
			if (id1) {
				err = got_object_open_as_tree(&subtree1, repo,
				    id1);
				if (err)
					goto done;
			}
			if (id2) {
				err = got_object_open_as_tree(&subtree2, repo,
				    id2);
				if (err)
					goto done;
			}
			err = got_diff_tree(subtree1, subtree2, f1, f2,
			    id1 ? pe->path : "/dev/null",
			    id2 ? pe->path : "/dev/null",
			    repo, cb, cb_arg, 1);
			if (err)
				goto done;
		} else {
			err = got_error(GOT_ERR_OBJ_TYPE);
			goto done;
		}
	}
done:
	free(id1);
	free(id2);
	if (subtree1)
		got_object_tree_close(subtree1);
	if (subtree2)
		got_object_tree_close(subtree2);
	if (blob1)
		got_object_blob_close(blob1);
	if (blob2)
		got_object_blob_close(blob2);
	return err;
}

static const struct got_error *
show_object_id(off_t **line_offsets, size_t *nlines, const char *obj_typestr,
    int ch, const char *id_str, FILE *outfile)
{
	const struct got_error *err;
	int n;
	off_t outoff = 0;

	n = fprintf(outfile, "%s %c %s\n", obj_typestr, ch, id_str);
	if (line_offsets != NULL && *line_offsets != NULL) {
		if (*nlines == 0) {
			err = add_line_offset(line_offsets, nlines, 0);
			if (err)
				return err;
		} else
			outoff = (*line_offsets)[*nlines - 1];

		outoff += n;
		err = add_line_offset(line_offsets, nlines, outoff);
		if (err)
			return err;
	}

	return NULL;
}

static const struct got_error *
diff_objects_as_trees(off_t **line_offsets, size_t *nlines,
    FILE *f1, FILE *f2, struct got_object_id *id1, struct got_object_id *id2,
    struct got_pathlist_head *paths, const char *label1, const char *label2,
    int diff_context, int ignore_whitespace, int force_text_diff,
    struct got_repository *repo, FILE *outfile)
{
	const struct got_error *err;
	struct got_tree_object *tree1 = NULL, *tree2 = NULL;
	struct got_diff_blob_output_unidiff_arg arg;
	int want_lineoffsets = (line_offsets != NULL && *line_offsets != NULL);

	if (id1 == NULL && id2 == NULL)
		return got_error(GOT_ERR_NO_OBJ);

	if (id1) {
		err = got_object_open_as_tree(&tree1, repo, id1);
		if (err)
			goto done;
	}
	if (id2) {
		err = got_object_open_as_tree(&tree2, repo, id2);
		if (err)
			goto done;
	}

	arg.diff_context = diff_context;
	arg.ignore_whitespace = ignore_whitespace;
	arg.force_text_diff = force_text_diff;
	arg.outfile = outfile;
	if (want_lineoffsets) {
		arg.line_offsets = *line_offsets;
		arg.nlines = *nlines;
	} else {
		arg.line_offsets = NULL;
		arg.nlines = 0;
	}
	if (paths == NULL || TAILQ_EMPTY(paths)) {
		err = got_diff_tree(tree1, tree2, f1, f2, label1, label2,
		    repo, got_diff_blob_output_unidiff, &arg, 1);
	} else {
		err = diff_paths(tree1, tree2, f1, f2, paths, repo,
		    got_diff_blob_output_unidiff, &arg);
	}
	if (want_lineoffsets) {
		*line_offsets = arg.line_offsets; /* was likely re-allocated */
		*nlines = arg.nlines;
	}
done:
	if (tree1)
		got_object_tree_close(tree1);
	if (tree2)
		got_object_tree_close(tree2);
	return err;
}

const struct got_error *
got_diff_objects_as_trees(off_t **line_offsets, size_t *nlines,
    FILE *f1, FILE *f2, struct got_object_id *id1, struct got_object_id *id2,
    struct got_pathlist_head *paths, const char *label1, const char *label2,
    int diff_context, int ignore_whitespace, int force_text_diff,
    struct got_repository *repo, FILE *outfile)
{
	const struct got_error *err;
	char *idstr = NULL;

	if (id1 == NULL && id2 == NULL)
		return got_error(GOT_ERR_NO_OBJ);

	if (id1) {
		err = got_object_id_str(&idstr, id1);
		if (err)
			goto done;
		err = show_object_id(line_offsets, nlines, "tree", '-',
		    idstr, outfile);
		if (err)
			goto done;
		free(idstr);
		idstr = NULL;
	} else {
		err = show_object_id(line_offsets, nlines, "tree", '-',
		    "/dev/null", outfile);
		if (err)
			goto done;
	}

	if (id2) {
		err = got_object_id_str(&idstr, id2);
		if (err)
			goto done;
		err = show_object_id(line_offsets, nlines, "tree", '+',
		    idstr, outfile);
		if (err)
			goto done;
		free(idstr);
		idstr = NULL;
	} else {
		err = show_object_id(line_offsets, nlines, "tree", '+',
		    "/dev/null", outfile);
		if (err)
			goto done;
	}

	err = diff_objects_as_trees(line_offsets, nlines, f1, f2, id1, id2,
	    paths, label1, label2, diff_context, ignore_whitespace,
	    force_text_diff, repo, outfile);
done:
	free(idstr);
	return err;
}

const struct got_error *
got_diff_objects_as_commits(off_t **line_offsets, size_t *nlines,
    FILE *f1, FILE *f2, struct got_object_id *id1, struct got_object_id *id2,
    struct got_pathlist_head *paths,
    int diff_context, int ignore_whitespace, int force_text_diff,
    struct got_repository *repo, FILE *outfile)
{
	const struct got_error *err;
	struct got_commit_object *commit1 = NULL, *commit2 = NULL;
	char *idstr = NULL;

	if (id2 == NULL)
		return got_error(GOT_ERR_NO_OBJ);

	if (id1) {
		err = got_object_open_as_commit(&commit1, repo, id1);
		if (err)
			goto done;
		err = got_object_id_str(&idstr, id1);
		if (err)
			goto done;
		err = show_object_id(line_offsets, nlines, "commit", '-',
		    idstr, outfile);
		if (err)
			goto done;
		free(idstr);
		idstr = NULL;
	} else {
		err = show_object_id(line_offsets, nlines, "commit", '-',
		    "/dev/null", outfile);
		if (err)
			goto done;
	}

	err = got_object_open_as_commit(&commit2, repo, id2);
	if (err)
		goto done;

	err = got_object_id_str(&idstr, id2);
	if (err)
		goto done;
	err = show_object_id(line_offsets, nlines, "commit", '+',
	    idstr, outfile);
	if (err)
		goto done;

	err = diff_objects_as_trees(line_offsets, nlines, f1, f2,
	    commit1 ? got_object_commit_get_tree_id(commit1) : NULL,
	    got_object_commit_get_tree_id(commit2), paths, "", "",
	    diff_context, ignore_whitespace, force_text_diff, repo, outfile);
done:
	if (commit1)
		got_object_commit_close(commit1);
	if (commit2)
		got_object_commit_close(commit2);
	free(idstr);
	return err;
}

const struct got_error *
got_diff_files(struct got_diffreg_result **resultp,
    FILE *f1, const char *label1, FILE *f2, const char *label2,
    int diff_context, int ignore_whitespace, int force_text_diff,
    FILE *outfile)
{
	const struct got_error *err = NULL;
	struct got_diffreg_result *diffreg_result = NULL;

	if (resultp)
		*resultp = NULL;

	if (outfile) {
		fprintf(outfile, "file - %s\n",
		    f1 == NULL ? "/dev/null" : label1);
		fprintf(outfile, "file + %s\n",
		    f2 == NULL ? "/dev/null" : label2);
	}

	err = got_diffreg(&diffreg_result, f1, f2, GOT_DIFF_ALGORITHM_PATIENCE,
	    ignore_whitespace, force_text_diff);
	if (err)
		goto done;

	if (outfile) {
		err = got_diffreg_output(NULL, NULL, diffreg_result,
		    f1 != NULL, f2 != NULL, label1, label2,
		    GOT_DIFF_OUTPUT_UNIDIFF, diff_context, outfile);
		if (err)
			goto done;
	}

done:
	if (resultp && err == NULL)
		*resultp = diffreg_result;
	else if (diffreg_result) {
		const struct got_error *free_err;
		free_err = got_diffreg_result_free(diffreg_result);
		if (free_err && err == NULL)
			err = free_err;
	}

	return err;
}
