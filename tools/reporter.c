/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2009 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "tools.h"
#include "report.h"

static int _process_each_devtype(struct cmd_context *cmd, int argc, void *handle)
{
	if (argc)
		log_warn("WARNING: devtypes currently ignores command line arguments.");

	if (!report_devtypes(handle))
		return_ECMD_FAILED;

	return ECMD_PROCESSED;
}

static int _vgs_single(struct cmd_context *cmd __attribute__((unused)),
		       const char *vg_name, struct volume_group *vg,
		       void *handle)
{
	if (!report_object(handle, vg, NULL, NULL, NULL, NULL, NULL, NULL))
		return_ECMD_FAILED;

	check_current_backup(vg);

	return ECMD_PROCESSED;
}

static int _lvs_single(struct cmd_context *cmd, struct logical_volume *lv,
		       void *handle)
{
	if (!report_object(handle, lv->vg, lv, NULL, NULL, NULL, NULL, NULL))
		return_ECMD_FAILED;

	return ECMD_PROCESSED;
}

static void _get_lv_info_for_report(struct cmd_context *cmd,
				   struct logical_volume *lv,
				   struct lvinfo *lvinfo)
{
	if (!lv_info(cmd, lv, 0, lvinfo, 1, 1))
		lvinfo->exists = 0;
}

static int _lvs_with_info_single(struct cmd_context *cmd, struct logical_volume *lv,
				 void *handle)
{
	struct lvinfo lvinfo;

	_get_lv_info_for_report(cmd, lv, &lvinfo);
	if (!report_object(handle, lv->vg, lv, NULL, NULL, NULL, &lvinfo, NULL))
		return_ECMD_FAILED;

	return ECMD_PROCESSED;
}

static int _segs_single(struct cmd_context *cmd __attribute__((unused)),
			struct lv_segment *seg, void *handle)
{
	if (!report_object(handle, seg->lv->vg, seg->lv, NULL, seg, NULL, NULL, NULL))
		return_ECMD_FAILED;

	return ECMD_PROCESSED;
}

static int _segs_with_lv_info_single(struct cmd_context *cmd __attribute__((unused)),
				     struct lv_segment *seg, void *handle)
{
	struct lvinfo lvinfo;

	_get_lv_info_for_report(cmd, seg->lv, &lvinfo);
	if (!report_object(handle, seg->lv->vg, seg->lv, NULL, seg, NULL, &lvinfo, NULL))
		return_ECMD_FAILED;

	return ECMD_PROCESSED;
}

static int _do_pvsegs_sub_single(struct cmd_context *cmd,
				 struct volume_group *vg,
				 struct pv_segment *pvseg,
				 int lv_info_needed,
				 void *handle)
{
	int ret = ECMD_PROCESSED;
	struct lv_segment *seg = pvseg->lvseg;
	struct lvinfo lvinfo = { .exists = 0 };

	struct segment_type _freeseg_type = {
		.cmd = cmd,
		.name = "free",
		.flags = SEG_VIRTUAL | SEG_CANNOT_BE_ZEROED,
	};

	struct volume_group _free_vg = {
		.cmd = cmd,
		.name = "",
		.pvs = DM_LIST_HEAD_INIT(_free_vg.pvs),
		.lvs = DM_LIST_HEAD_INIT(_free_vg.lvs),
		.tags = DM_LIST_HEAD_INIT(_free_vg.tags),
	};

	struct logical_volume _free_logical_volume = {
		.vg = vg ?: &_free_vg,
		.name = "",
		.status = VISIBLE_LV,
		.major = -1,
		.minor = -1,
		.tags = DM_LIST_HEAD_INIT(_free_logical_volume.tags),
		.segments = DM_LIST_HEAD_INIT(_free_logical_volume.segments),
		.segs_using_this_lv = DM_LIST_HEAD_INIT(_free_logical_volume.segs_using_this_lv),
		.snapshot_segs = DM_LIST_HEAD_INIT(_free_logical_volume.snapshot_segs),
	};

	struct lv_segment _free_lv_segment = {
		.lv = &_free_logical_volume,
		.segtype = &_freeseg_type,
		.len = pvseg->len,
		.tags = DM_LIST_HEAD_INIT(_free_lv_segment.tags),
		.origin_list = DM_LIST_HEAD_INIT(_free_lv_segment.origin_list),
	};

	if (seg && lv_info_needed)
	    _get_lv_info_for_report(cmd, seg->lv, &lvinfo);

	if (!report_object(handle, vg, seg ? seg->lv : &_free_logical_volume, pvseg->pv,
			   seg ? : &_free_lv_segment, pvseg, &lvinfo, pv_label(pvseg->pv))) {
		ret = ECMD_FAILED;
		goto_out;
	}

 out:
	return ret;
}

static int _pvsegs_sub_single(struct cmd_context *cmd,
			      struct volume_group *vg,
			      struct pv_segment *pvseg, void *handle)
{
	return _do_pvsegs_sub_single(cmd, vg, pvseg, 0, handle);
}

static int _pvsegs_with_lv_info_sub_single(struct cmd_context *cmd,
					   struct volume_group *vg,
					   struct pv_segment *pvseg,
					   void *handle)
{
	return _do_pvsegs_sub_single(cmd, vg, pvseg, 1, handle);
}

static int _lvsegs_single(struct cmd_context *cmd, struct logical_volume *lv,
			  void *handle)
{
	if (!arg_count(cmd, all_ARG) && !lv_is_visible(lv))
		return ECMD_PROCESSED;

	return process_each_segment_in_lv(cmd, lv, handle, _segs_single);
}

static int _lvsegs_with_lv_info_single(struct cmd_context *cmd,
				       struct logical_volume *lv,
				       void *handle)
{
	if (!arg_count(cmd, all_ARG) && !lv_is_visible(lv))
		return ECMD_PROCESSED;

	return process_each_segment_in_lv(cmd, lv, handle, _segs_with_lv_info_single);
}

static int _pvsegs_single(struct cmd_context *cmd, struct volume_group *vg,
			  struct physical_volume *pv, void *handle)
{
	return process_each_segment_in_pv(cmd, vg, pv, handle,
					  _pvsegs_sub_single);
}

static int _pvsegs_with_lv_info_single(struct cmd_context *cmd,
				       struct volume_group *vg,
				       struct physical_volume *pv,
				       void *handle)
{
	return process_each_segment_in_pv(cmd, vg, pv, handle,
				_pvsegs_with_lv_info_sub_single);
}

static int _pvs_single(struct cmd_context *cmd, struct volume_group *vg,
		       struct physical_volume *pv, void *handle)
{
	if (!report_object(handle, vg, NULL, pv, NULL, NULL, NULL, NULL))
		return_ECMD_FAILED;

	return ECMD_PROCESSED;
}

static int _label_single(struct cmd_context *cmd, struct label *label,
		         void *handle)
{
	if (!report_object(handle, NULL, NULL, NULL, NULL, NULL, NULL, label))
		return_ECMD_FAILED;

	return ECMD_PROCESSED;
}

static int _pvs_in_vg(struct cmd_context *cmd, const char *vg_name,
		      struct volume_group *vg,
		      void *handle)
{
	return process_each_pv_in_vg(cmd, vg, handle, &_pvs_single);
}

static int _pvsegs_in_vg(struct cmd_context *cmd, const char *vg_name,
			 struct volume_group *vg,
			 void *handle)
{
	return process_each_pv_in_vg(cmd, vg, handle, &_pvsegs_single);
}

static int _report(struct cmd_context *cmd, int argc, char **argv,
		   report_type_t report_type)
{
	void *report_handle;
	const char *opts;
	char *str;
	const char *keys = NULL, *options = NULL, *selection = NULL, *separator;
	int r = ECMD_PROCESSED;
	int aligned, buffered, headings, field_prefixes, quoted;
	int columns_as_rows;
	unsigned args_are_pvs, lv_info_needed;
	int lock_global = 0;

	/*
	 * When reporting foreign VGs we want to refresh our cached
	 * copy of them, since other hosts have probably made changes
	 * to their own VGs.  We also want to override the default
	 * behavior which skips over foreign VGs.
	 */
	if (arg_is_set(cmd, foreign_ARG) && lvmetad_used()) {
		lvmetad_pvscan_all_devs(cmd, NULL);
		cmd->include_foreign_vgs = 1;
	}

	aligned = find_config_tree_bool(cmd, report_aligned_CFG, NULL);
	buffered = find_config_tree_bool(cmd, report_buffered_CFG, NULL);
	headings = find_config_tree_bool(cmd, report_headings_CFG, NULL);
	separator = find_config_tree_str(cmd, report_separator_CFG, NULL);
	field_prefixes = find_config_tree_bool(cmd, report_prefixes_CFG, NULL);
	quoted = find_config_tree_bool(cmd, report_quoted_CFG, NULL);
	columns_as_rows = find_config_tree_bool(cmd, report_colums_as_rows_CFG, NULL);

	args_are_pvs = (report_type == PVS ||
			report_type == LABEL ||
			report_type == PVSEGS) ? 1 : 0;

	/*
	 * FIXME Trigger scans based on unrecognised listed devices instead.
	 */
	if (args_are_pvs && argc)
		cmd->filter->wipe(cmd->filter);

	switch (report_type) {
	case DEVTYPES:
		keys = find_config_tree_str(cmd, report_devtypes_sort_CFG, NULL);
		if (!arg_count(cmd, verbose_ARG))
			options = find_config_tree_str(cmd, report_devtypes_cols_CFG, NULL);
		else
			options = find_config_tree_str(cmd, report_devtypes_cols_verbose_CFG, NULL);
		break;
	case LVS:
		keys = find_config_tree_str(cmd, report_lvs_sort_CFG, NULL);
		if (!arg_count(cmd, verbose_ARG))
			options = find_config_tree_str(cmd, report_lvs_cols_CFG, NULL);
		else
			options = find_config_tree_str(cmd, report_lvs_cols_verbose_CFG, NULL);
		break;
	case VGS:
		keys = find_config_tree_str(cmd, report_vgs_sort_CFG, NULL);
		if (!arg_count(cmd, verbose_ARG))
			options = find_config_tree_str(cmd, report_vgs_cols_CFG, NULL);
		else
			options = find_config_tree_str(cmd, report_vgs_cols_verbose_CFG, NULL);
		break;
	case LABEL:
	case PVS:
		keys = find_config_tree_str(cmd, report_pvs_sort_CFG, NULL);
		if (!arg_count(cmd, verbose_ARG))
			options = find_config_tree_str(cmd, report_pvs_cols_CFG, NULL);
		else
			options = find_config_tree_str(cmd, report_pvs_cols_verbose_CFG, NULL);
		break;
	case SEGS:
		keys = find_config_tree_str(cmd, report_segs_sort_CFG, NULL);
		if (!arg_count(cmd, verbose_ARG))
			options = find_config_tree_str(cmd, report_segs_cols_CFG, NULL);
		else
			options = find_config_tree_str(cmd, report_segs_cols_verbose_CFG, NULL);
		break;
	case PVSEGS:
		keys = find_config_tree_str(cmd, report_pvsegs_sort_CFG, NULL);
		if (!arg_count(cmd, verbose_ARG))
			options = find_config_tree_str(cmd, report_pvsegs_cols_CFG, NULL);
		else
			options = find_config_tree_str(cmd, report_pvsegs_cols_verbose_CFG, NULL);
		break;
	default:
		log_error(INTERNAL_ERROR "Unknown report type.");
		return ECMD_FAILED;
	}

	/* If -o supplied use it, else use default for report_type */
	if (arg_count(cmd, options_ARG)) {
		opts = arg_str_value(cmd, options_ARG, "");
		if (!opts || !*opts) {
			log_error("Invalid options string: %s", opts);
			return EINVALID_CMD_LINE;
		}
		if (*opts == '+') {
			if (!(str = dm_pool_alloc(cmd->mem,
					 strlen(options) + strlen(opts) + 1))) {
				log_error("options string allocation failed");
				return ECMD_FAILED;
			}
			(void) sprintf(str, "%s,%s", options, opts + 1);
			options = str;
		} else
			options = opts;
	}

	/* -O overrides default sort settings */
	keys = arg_str_value(cmd, sort_ARG, keys);

	separator = arg_str_value(cmd, separator_ARG, separator);
	if (arg_count(cmd, separator_ARG))
		aligned = 0;
	if (arg_count(cmd, aligned_ARG))
		aligned = 1;
	if (arg_count(cmd, unbuffered_ARG) && !arg_count(cmd, sort_ARG))
		buffered = 0;
	if (arg_count(cmd, noheadings_ARG))
		headings = 0;
	if (arg_count(cmd, nameprefixes_ARG)) {
		aligned = 0;
		field_prefixes = 1;
	}
	if (arg_count(cmd, unquoted_ARG))
		quoted = 0;
	if (arg_count(cmd, rows_ARG))
		columns_as_rows = 1;

	if (arg_count(cmd, select_ARG))
		selection = arg_str_value(cmd, select_ARG, NULL);

	if (!(report_handle = report_init(cmd, options, keys, &report_type,
					  separator, aligned, buffered,
					  headings, field_prefixes, quoted,
					  columns_as_rows, selection)))
		return_ECMD_FAILED;

	/* Do we need lv_info to be called for LV device status? */
	lv_info_needed = (report_type & LVSINFO) ? 1 : 0;

	/* Ensure options selected are compatible */
	if (report_type & SEGS)
		report_type |= LVS;
	if (report_type & PVSEGS)
		report_type |= PVS;
	if ((report_type & (LVS | LVSINFO)) && (report_type & (PVS | LABEL)) && !args_are_pvs) {
		log_error("Can't report LV and PV fields at the same time");
		dm_report_free(report_handle);
		return ECMD_FAILED;
	}

	/* Change report type if fields specified makes this necessary */
	if ((report_type & PVSEGS) ||
	    ((report_type & (PVS | LABEL)) && (report_type & (LVS | LVSINFO))))
		report_type = PVSEGS;
	else if ((report_type & LABEL) && (report_type & VGS))
		report_type = PVS;
	else if (report_type & PVS)
		report_type = PVS;
	else if (report_type & SEGS)
		report_type = SEGS;
	else if (report_type & LVSINFO)
		report_type = LVSINFO;
	else if (report_type & LVS)
		report_type = LVS;

	/*
	 * We lock VG_GLOBAL to enable use of metadata cache.
	 * This can pause alongide pvscan or vgscan process for a while.
	 */
	if (args_are_pvs && (report_type == PVS || report_type == PVSEGS) &&
	    !lvmetad_active()) {
		lock_global = 1;
		if (!lock_vol(cmd, VG_GLOBAL, LCK_VG_READ, NULL)) {
			log_error("Unable to obtain global lock.");
			return ECMD_FAILED;
		}
	}

	switch (report_type) {
	case DEVTYPES:
		r = _process_each_devtype(cmd, argc, report_handle);
		break;
	case LVS:
		r = process_each_lv(cmd, argc, argv, 0, report_handle,
				    &_lvs_single);
		break;
	case LVSINFO:
		r = process_each_lv(cmd, argc, argv, 0, report_handle,
				    &_lvs_with_info_single);
		break;
	case VGS:
		r = process_each_vg(cmd, argc, argv, 0,
				    report_handle, &_vgs_single);
		break;
	case LABEL:
		r = process_each_label(cmd, argc, argv,
				       report_handle, &_label_single);
		break;
	case PVS:
		if (args_are_pvs)
			r = process_each_pv(cmd, argc, argv, NULL, 0,
					    report_handle, &_pvs_single);
		else
			r = process_each_vg(cmd, argc, argv, 0,
					    report_handle, &_pvs_in_vg);
		break;
	case SEGS:
		r = process_each_lv(cmd, argc, argv, 0, report_handle,
				    lv_info_needed ? &_lvsegs_with_lv_info_single
						   : &_lvsegs_single);
		break;
	case PVSEGS:
		if (args_are_pvs)
			r = process_each_pv(cmd, argc, argv, NULL, 0,
					    report_handle,
					    lv_info_needed ? &_pvsegs_with_lv_info_single
							   : &_pvsegs_single);
		else
			r = process_each_vg(cmd, argc, argv, 0,
					    report_handle, &_pvsegs_in_vg);
		break;
	}

	dm_report_output(report_handle);

	dm_report_free(report_handle);

	if (lock_global)
		unlock_vg(cmd, VG_GLOBAL);

	return r;
}

int lvs(struct cmd_context *cmd, int argc, char **argv)
{
	report_type_t type;

	if (arg_count(cmd, segments_ARG))
		type = SEGS;
	else
		type = LVS;

	return _report(cmd, argc, argv, type);
}

int vgs(struct cmd_context *cmd, int argc, char **argv)
{
	return _report(cmd, argc, argv, VGS);
}

int pvs(struct cmd_context *cmd, int argc, char **argv)
{
	report_type_t type;

	if (arg_count(cmd, segments_ARG))
		type = PVSEGS;
	else
		type = LABEL;

	return _report(cmd, argc, argv, type);
}

int devtypes(struct cmd_context *cmd, int argc, char **argv)
{
	return _report(cmd, argc, argv, DEVTYPES);
}
