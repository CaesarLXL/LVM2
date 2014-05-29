/*
 * Copyright (C) 2002-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2007 Red Hat, Inc. All rights reserved.
 *
 * This file is part of the device-mapper userspace tools.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "dmlib.h"

#include <ctype.h>

/*
 * Internal flags
 */
#define RH_SORT_REQUIRED	0x00000100
#define RH_HEADINGS_PRINTED	0x00000200

struct dm_report {
	struct dm_pool *mem;

	/* To report all available types */
#define REPORT_TYPES_ALL	UINT32_MAX
	uint32_t report_types;
	const char *output_field_name_prefix;
	const char *field_prefix;
	uint32_t flags;
	const char *separator;

	uint32_t keys_count;

	/* Ordered list of fields needed for this report */
	struct dm_list field_props;

	/* Rows of report data */
	struct dm_list rows;

	/* Array of field definitions */
	const struct dm_report_field_type *fields;
	const struct dm_report_object_type *types;

	/* To store caller private data */
	void *private;
};

/*
 * Internal per-field flags
 */
#define FLD_HIDDEN	0x00000100
#define FLD_SORT_KEY	0x00000200
#define FLD_ASCENDING	0x00000400
#define FLD_DESCENDING	0x00000800

struct field_properties {
	struct dm_list list;
	uint32_t field_num;
	uint32_t sort_posn;
	int32_t width;
	const struct dm_report_object_type *type;
	uint32_t flags;
};

/*
 * Report selection
 */
struct op_def {
	const char *string;
	uint32_t flags;
	const char *desc;
};

#define FLD_CMP_MASK	0x000FF000
#define FLD_CMP_EQUAL	0x00001000
#define FLD_CMP_NOT	0x00002000
#define FLD_CMP_GT	0x00004000
#define FLD_CMP_LT	0x00008000
#define FLD_CMP_REGEX	0x00010000
#define FLD_CMP_NUMBER  0x00020000

/*
 * When defining operators, always define longer one before
 * shorter one if one is a prefix of another!
 * (e.g. =~ comes before =)
*/
static struct op_def _op_cmp[] = {
	{ "=~", FLD_CMP_REGEX, "Matching regular expression" },
	{ "!~", FLD_CMP_REGEX|FLD_CMP_NOT, "Not matching regular expression" },
	{ "=", FLD_CMP_EQUAL, "Equal to" },
	{ "!=", FLD_CMP_NOT|FLD_CMP_EQUAL, "Not equal" },
	{ ">=", FLD_CMP_NUMBER|FLD_CMP_GT|FLD_CMP_EQUAL, "Greater than or equal to" },
	{ ">", FLD_CMP_NUMBER|FLD_CMP_GT, "Greater than" },
	{ "<=", FLD_CMP_NUMBER|FLD_CMP_LT|FLD_CMP_EQUAL, "Lesser than or equal to" },
	{ "<", FLD_CMP_NUMBER|FLD_CMP_LT, "Lesser than" },
	{ NULL, 0, NULL }
};

#define SEL_MASK		0x000000FF
#define SEL_ITEM		0x00000001
#define SEL_AND 		0x00000002
#define SEL_OR			0x00000004

#define SEL_MODIFIER_MASK	0x00000F00
#define SEL_MODIFIER_NOT	0x00000100

#define SEL_PRECEDENCE_MASK	0x0000F000
#define SEL_PRECEDENCE_PS	0x00001000
#define SEL_PRECEDENCE_PE	0x00002000

static struct op_def _op_log[] = {
        { "&&", SEL_AND, "Logical conjunction" },
	{ ",", SEL_AND, "Logical conjunction (alternative)" },
        { "||", SEL_OR, "Logical disjunction" },
	{ "/", SEL_OR, "Logical disjunction (alternative)" },
        { "!", SEL_MODIFIER_NOT, "Logical negation" },
        { "(", SEL_PRECEDENCE_PS, "Left parenthesis" },
        { ")", SEL_PRECEDENCE_PE, "Right parenthesis" },
        { NULL,  0, NULL},
};

struct field_selection {
	struct field_properties *fp;
	uint32_t flags;
	union {
		const char *s;
		uint64_t i;
		double d;
		struct dm_regex *r;
	} v;
};

struct selection_node {
	struct dm_list list;
	uint32_t type;
	union {
		struct field_selection *item;
		struct dm_list set;
	} selection;
};

/*
 * Report data field
 */
struct dm_report_field {
	struct dm_list list;
	struct field_properties *props;

	const char *report_string;	/* Formatted ready for display */
	const void *sort_value;		/* Raw value for sorting */
};

struct row {
	struct dm_list list;
	struct dm_report *rh;
	struct dm_list fields;			  /* Fields in display order */
	struct dm_report_field *(*sort_fields)[]; /* Fields in sort order */
};

static const struct dm_report_object_type *_find_type(struct dm_report *rh,
						      uint32_t report_type)
{
	const struct dm_report_object_type *t;

	for (t = rh->types; t->data_fn; t++)
		if (t->id == report_type)
			return t;

	return NULL;
}

/*
 * Data-munging functions to prepare each data type for display and sorting
 */

int dm_report_field_string(struct dm_report *rh,
			   struct dm_report_field *field, const char *const *data)
{
	char *repstr;

	if (!(repstr = dm_pool_strdup(rh->mem, *data))) {
		log_error("dm_report_field_string: dm_pool_strdup failed");
		return 0;
	}

	field->report_string = repstr;
	field->sort_value = (const void *) field->report_string;

	return 1;
}

int dm_report_field_int(struct dm_report *rh,
			struct dm_report_field *field, const int *data)
{
	const int value = *data;
	uint64_t *sortval;
	char *repstr;

	if (!(repstr = dm_pool_zalloc(rh->mem, 13))) {
		log_error("dm_report_field_int: dm_pool_alloc failed");
		return 0;
	}

	if (!(sortval = dm_pool_alloc(rh->mem, sizeof(int64_t)))) {
		log_error("dm_report_field_int: dm_pool_alloc failed");
		return 0;
	}

	if (dm_snprintf(repstr, 12, "%d", value) < 0) {
		log_error("dm_report_field_int: int too big: %d", value);
		return 0;
	}

	*sortval = (uint64_t) value;
	field->sort_value = sortval;
	field->report_string = repstr;

	return 1;
}

int dm_report_field_uint32(struct dm_report *rh,
			   struct dm_report_field *field, const uint32_t *data)
{
	const uint32_t value = *data;
	uint64_t *sortval;
	char *repstr;

	if (!(repstr = dm_pool_zalloc(rh->mem, 12))) {
		log_error("dm_report_field_uint32: dm_pool_alloc failed");
		return 0;
	}

	if (!(sortval = dm_pool_alloc(rh->mem, sizeof(uint64_t)))) {
		log_error("dm_report_field_uint32: dm_pool_alloc failed");
		return 0;
	}

	if (dm_snprintf(repstr, 11, "%u", value) < 0) {
		log_error("dm_report_field_uint32: uint32 too big: %u", value);
		return 0;
	}

	*sortval = (uint64_t) value;
	field->sort_value = sortval;
	field->report_string = repstr;

	return 1;
}

int dm_report_field_int32(struct dm_report *rh,
			  struct dm_report_field *field, const int32_t *data)
{
	const int32_t value = *data;
	uint64_t *sortval;
	char *repstr;

	if (!(repstr = dm_pool_zalloc(rh->mem, 13))) {
		log_error("dm_report_field_int32: dm_pool_alloc failed");
		return 0;
	}

	if (!(sortval = dm_pool_alloc(rh->mem, sizeof(int64_t)))) {
		log_error("dm_report_field_int32: dm_pool_alloc failed");
		return 0;
	}

	if (dm_snprintf(repstr, 12, "%d", value) < 0) {
		log_error("dm_report_field_int32: int32 too big: %d", value);
		return 0;
	}

	*sortval = (uint64_t) value;
	field->sort_value = sortval;
	field->report_string = repstr;

	return 1;
}

int dm_report_field_uint64(struct dm_report *rh,
			   struct dm_report_field *field, const uint64_t *data)
{
	const uint64_t value = *data;
	uint64_t *sortval;
	char *repstr;

	if (!(repstr = dm_pool_zalloc(rh->mem, 22))) {
		log_error("dm_report_field_uint64: dm_pool_alloc failed");
		return 0;
	}

	if (!(sortval = dm_pool_alloc(rh->mem, sizeof(uint64_t)))) {
		log_error("dm_report_field_uint64: dm_pool_alloc failed");
		return 0;
	}

	if (dm_snprintf(repstr, 21, "%" PRIu64 , value) < 0) {
		log_error("dm_report_field_uint64: uint64 too big: %" PRIu64, value);
		return 0;
	}

	*sortval = value;
	field->sort_value = sortval;
	field->report_string = repstr;

	return 1;
}

/*
 * Helper functions for custom report functions
 */
void dm_report_field_set_value(struct dm_report_field *field, const void *value, const void *sortvalue)
{
	field->report_string = (const char *) value;
	field->sort_value = sortvalue ? : value;

	if ((field->sort_value == value) &&
	    (field->props->flags & DM_REPORT_FIELD_TYPE_NUMBER))
		log_warn(INTERNAL_ERROR "Using string as sort value for numerical field.");
}

/*
 * show help message
 */
static void _display_fields(struct dm_report *rh)
{
	uint32_t f;
	const struct dm_report_object_type *type;
	const char *desc, *last_desc = "";
	size_t id_len = 0;

	for (f = 0; rh->fields[f].report_fn; f++)
		if (strlen(rh->fields[f].id) > id_len)
			id_len = strlen(rh->fields[f].id);


	for (type = rh->types; type->data_fn; type++)
		if (strlen(type->prefix) + 3 > id_len)
			id_len = strlen(type->prefix) + 3;

	for (f = 0; rh->fields[f].report_fn; f++) {
		if ((type = _find_type(rh, rh->fields[f].type)) && type->desc)
			desc = type->desc;
		else
			desc = " ";
		if (desc != last_desc) {
			if (*last_desc)
				log_warn(" ");
			log_warn("%s Fields", desc);
			log_warn("%*.*s", (int) strlen(desc) + 7,
				 (int) strlen(desc) + 7,
				 "-------------------------------------------------------------------------------");
			log_warn("  %sall%-*s - %s", type->prefix,
				 (int) (id_len - 3 - strlen(type->prefix)), "",
				 "All fields in this section.");
		}

		/* FIXME Add line-wrapping at terminal width (or 80 cols) */
		log_warn("  %-*s - %s", (int) id_len, rh->fields[f].id, rh->fields[f].desc);
		last_desc = desc;
	}
}

/*
 * Initialise report handle
 */
static int _copy_field(struct dm_report *rh, struct field_properties *dest,
		       uint32_t field_num)
{
	dest->field_num = field_num;
	dest->width = rh->fields[field_num].width;
	dest->flags = rh->fields[field_num].flags & DM_REPORT_FIELD_MASK;

	/* set object type method */
	dest->type = _find_type(rh, rh->fields[field_num].type);
	if (!dest->type) {
		log_error("dm_report: field not match: %s",
			  rh->fields[field_num].id);
		return 0;
	}

	return 1;
}

static struct field_properties * _add_field(struct dm_report *rh,
					    uint32_t field_num, uint32_t flags)
{
	struct field_properties *fp;

	if (!(fp = dm_pool_zalloc(rh->mem, sizeof(struct field_properties)))) {
		log_error("dm_report: struct field_properties allocation "
			  "failed");
		return NULL;
	}

	if (!_copy_field(rh, fp, field_num)) {
		stack;
		dm_pool_free(rh->mem, fp);
		return NULL;
	}

	fp->flags |= flags;

	/*
	 * Place hidden fields at the front so dm_list_end() will
	 * tell us when we've reached the last visible field.
	 */
	if (fp->flags & FLD_HIDDEN)
		dm_list_add_h(&rh->field_props, &fp->list);
	else
		dm_list_add(&rh->field_props, &fp->list);

	return fp;
}

/*
 * Compare name1 against name2 or prefix plus name2
 * name2 is not necessarily null-terminated.
 * len2 is the length of name2.
 */
static int _is_same_field(const char *name1, const char *name2,
			  size_t len2, const char *prefix)
{
	size_t prefix_len;

	/* Exact match? */
	if (!strncasecmp(name1, name2, len2) && strlen(name1) == len2)
		return 1;

	/* Match including prefix? */
	prefix_len = strlen(prefix);
	if (!strncasecmp(prefix, name1, prefix_len) &&
	    !strncasecmp(name1 + prefix_len, name2, len2) &&
	    strlen(name1) == prefix_len + len2)
		return 1;

	return 0;
}

/*
 * Check for a report type prefix + "all" match.
 */
static uint32_t _all_match(struct dm_report *rh, const char *field, size_t flen)
{
	size_t prefix_len;
	const struct dm_report_object_type *t;
	uint32_t report_types = 0;
	unsigned unprefixed_all_matched = 0;

	if (!strncasecmp(field, "all", 3) && flen == 3) {
		/* If there's no report prefix, match all report types */
		if (!(flen = strlen(rh->field_prefix)))
			return rh->report_types ? : REPORT_TYPES_ALL;

		/* otherwise include all fields beginning with the report prefix. */
		unprefixed_all_matched = 1;
		field = rh->field_prefix;
		report_types = rh->report_types;
	}

	/* Combine all report types that have a matching prefix. */
	for (t = rh->types; t->data_fn; t++) {
		prefix_len = strlen(t->prefix);

		if (!strncasecmp(t->prefix, field, prefix_len) &&
		    ((unprefixed_all_matched && (flen == prefix_len)) ||
		     (!strncasecmp(field + prefix_len, "all", 3) &&
		      (flen == prefix_len + 3))))
			report_types |= t->id;
	}

	return report_types;
}

/*
 * Add all fields with a matching type.
 */
static int _add_all_fields(struct dm_report *rh, uint32_t type)
{
	uint32_t f;

	for (f = 0; rh->fields[f].report_fn; f++)
		if ((rh->fields[f].type & type) && !_add_field(rh, f, 0))
			return 0;

	return 1;
}

static int _field_match(struct dm_report *rh, const char *field, size_t flen,
			unsigned report_type_only)
{
	uint32_t f, type;

	if (!flen)
		return 0;

	for (f = 0; rh->fields[f].report_fn; f++)
		if (_is_same_field(rh->fields[f].id, field, flen,
				   rh->field_prefix)) {
			if (report_type_only) {
				rh->report_types |= rh->fields[f].type;
				return 1;
			} else
				return _add_field(rh, f, 0) ? 1 : 0;
		}

	if ((type = _all_match(rh, field, flen))) {
		if (report_type_only) {
			rh->report_types |= type;
			return 1;
		} else
			return  _add_all_fields(rh, type);
	}

	return 0;
}

static int _add_sort_key(struct dm_report *rh, uint32_t field_num,
			 uint32_t flags, unsigned report_type_only)
{
	struct field_properties *fp, *found = NULL;

	dm_list_iterate_items(fp, &rh->field_props) {
		if (fp->field_num == field_num) {
			found = fp;
			break;
		}
	}

	if (!found) {
		if (report_type_only)
			rh->report_types |= rh->fields[field_num].type;
		else if (!(found = _add_field(rh, field_num, FLD_HIDDEN)))
			return_0;
	}

	if (report_type_only)
		return 1;

	if (found->flags & FLD_SORT_KEY) {
		log_warn("dm_report: Ignoring duplicate sort field: %s.",
			 rh->fields[field_num].id);
		return 1;
	}

	found->flags |= FLD_SORT_KEY;
	found->sort_posn = rh->keys_count++;
	found->flags |= flags;

	return 1;
}

static int _key_match(struct dm_report *rh, const char *key, size_t len,
		      unsigned report_type_only)
{
	uint32_t f;
	uint32_t flags;

	if (!len)
		return 0;

	if (*key == '+') {
		key++;
		len--;
		flags = FLD_ASCENDING;
	} else if (*key == '-') {
		key++;
		len--;
		flags = FLD_DESCENDING;
	} else
		flags = FLD_ASCENDING;

	if (!len) {
		log_error("dm_report: Missing sort field name");
		return 0;
	}

	for (f = 0; rh->fields[f].report_fn; f++)
		if (_is_same_field(rh->fields[f].id, key, len,
				   rh->field_prefix))
			return _add_sort_key(rh, f, flags, report_type_only);

	return 0;
}

static int _parse_fields(struct dm_report *rh, const char *format,
			 unsigned report_type_only)
{
	const char *ws;		/* Word start */
	const char *we = format;	/* Word end */

	while (*we) {
		/* Allow consecutive commas */
		while (*we && *we == ',')
			we++;

		/* start of the field name */
		ws = we;
		while (*we && *we != ',')
			we++;

		if (!_field_match(rh, ws, (size_t) (we - ws), report_type_only)) {
			_display_fields(rh);
			log_warn(" ");
			if (strcasecmp(ws, DM_REPORT_FIELD_RESERVED_NAME_HELP) &&
			    strcmp(ws, DM_REPORT_FIELD_RESERVED_NAME_HELP_ALT))
				log_error("Unrecognised field: %.*s",
					  (int) (we - ws), ws);
			return 0;
		}
	}

	return 1;
}

static int _parse_keys(struct dm_report *rh, const char *keys,
		       unsigned report_type_only)
{
	const char *ws;		/* Word start */
	const char *we = keys;	/* Word end */

	if (!keys)
		return 1;

	while (*we) {
		/* Allow consecutive commas */
		while (*we && *we == ',')
			we++;
		ws = we;
		while (*we && *we != ',')
			we++;
		if (!_key_match(rh, ws, (size_t) (we - ws), report_type_only)) {
			_display_fields(rh);
			log_warn(" ");
			if (strcasecmp(ws, DM_REPORT_FIELD_RESERVED_NAME_HELP) &&
			    strcmp(ws, DM_REPORT_FIELD_RESERVED_NAME_HELP_ALT))
				log_error("dm_report: Unrecognised field: %.*s",
					  (int) (we - ws), ws);
			return 0;
		}
	}

	return 1;
}

struct dm_report *dm_report_init(uint32_t *report_types,
				 const struct dm_report_object_type *types,
				 const struct dm_report_field_type *fields,
				 const char *output_fields,
				 const char *output_separator,
				 uint32_t output_flags,
				 const char *sort_keys,
				 void *private_data)
{
	struct dm_report *rh;
	const struct dm_report_object_type *type;

	if (!(rh = dm_zalloc(sizeof(*rh)))) {
		log_error("dm_report_init: dm_malloc failed");
		return 0;
	}

	/*
	 * rh->report_types is updated in _parse_fields() and _parse_keys()
	 * to contain all types corresponding to the fields specified by
	 * fields or keys.
	 */
	if (report_types)
		rh->report_types = *report_types;

	rh->separator = output_separator;
	rh->fields = fields;
	rh->types = types;
	rh->private = private_data;

	rh->flags |= output_flags & DM_REPORT_OUTPUT_MASK;

	/* With columns_as_rows we must buffer and not align. */
	if (output_flags & DM_REPORT_OUTPUT_COLUMNS_AS_ROWS) {
		if (!(output_flags & DM_REPORT_OUTPUT_BUFFERED))
			rh->flags |= DM_REPORT_OUTPUT_BUFFERED;
		if (output_flags & DM_REPORT_OUTPUT_ALIGNED)
			rh->flags &= ~DM_REPORT_OUTPUT_ALIGNED;
	}

	if (output_flags & DM_REPORT_OUTPUT_BUFFERED)
		rh->flags |= RH_SORT_REQUIRED;

	dm_list_init(&rh->field_props);
	dm_list_init(&rh->rows);

	if ((type = _find_type(rh, rh->report_types)) && type->prefix)
		rh->field_prefix = type->prefix;
	else
		rh->field_prefix = "";

	if (!(rh->mem = dm_pool_create("report", 10 * 1024))) {
		log_error("dm_report_init: allocation of memory pool failed");
		dm_free(rh);
		return NULL;
	}

	/*
	 * To keep the code needed to add the "all" field to a minimum, we parse
	 * the field lists twice.  The first time we only update the report type.
	 * FIXME Use one pass instead and expand the "all" field afterwards.
	 */
	if (!_parse_fields(rh, output_fields, 1) ||
	    !_parse_keys(rh, sort_keys, 1)) {
		dm_report_free(rh);
		return NULL;
	}

	/* Generate list of fields for output based on format string & flags */
	if (!_parse_fields(rh, output_fields, 0) ||
	    !_parse_keys(rh, sort_keys, 0)) {
		dm_report_free(rh);
		return NULL;
	}

	/* Return updated types value for further compatility check by caller */
	if (report_types)
		*report_types = rh->report_types;

	return rh;
}

void dm_report_free(struct dm_report *rh)
{
	dm_pool_destroy(rh->mem);
	dm_free(rh);
}

static char *_toupperstr(char *str)
{
	char *u = str;

	do
		*u = toupper(*u);
	while (*u++);

	return str;
}

int dm_report_set_output_field_name_prefix(struct dm_report *rh, const char *output_field_name_prefix)
{
	char *prefix;

	if (!(prefix = dm_pool_strdup(rh->mem, output_field_name_prefix))) {
		log_error("dm_report_set_output_field_name_prefix: dm_pool_strdup failed");
		return 0;
	}

	rh->output_field_name_prefix = _toupperstr(prefix);
	
	return 1;
}

/*
 * Create a row of data for an object
 */
static void *_report_get_field_data(struct dm_report *rh,
				    struct field_properties *fp, void *object)
{
	char *ret = fp->type->data_fn(object);

	if (!ret)
		return NULL;

	return (void *)(ret + rh->fields[fp->field_num].offset);
}

int dm_report_object(struct dm_report *rh, void *object)
{
	struct field_properties *fp;
	struct row *row;
	struct dm_report_field *field;
	void *data = NULL;

	if (!rh) {
		log_error(INTERNAL_ERROR "dm_report handler is NULL.");
		return 0;
	}

	if (!(row = dm_pool_zalloc(rh->mem, sizeof(*row)))) {
		log_error("dm_report_object: struct row allocation failed");
		return 0;
	}

	row->rh = rh;

	if ((rh->flags & RH_SORT_REQUIRED) &&
	    !(row->sort_fields =
		dm_pool_zalloc(rh->mem, sizeof(struct dm_report_field *) *
			       rh->keys_count))) {
		log_error("dm_report_object: "
			  "row sort value structure allocation failed");
		return 0;
	}

	dm_list_init(&row->fields);
	dm_list_add(&rh->rows, &row->list);

	/* For each field to be displayed, call its report_fn */
	dm_list_iterate_items(fp, &rh->field_props) {
		if (!(field = dm_pool_zalloc(rh->mem, sizeof(*field)))) {
			log_error("dm_report_object: "
				  "struct dm_report_field allocation failed");
			return 0;
		}
		field->props = fp;

		data = _report_get_field_data(rh, fp, object);
		if (!data)
			return 0;

		if (!rh->fields[fp->field_num].report_fn(rh, rh->mem,
							 field, data,
							 rh->private)) {
			log_error("dm_report_object: "
				  "report function failed for field %s",
				  rh->fields[fp->field_num].id);
			return 0;
		}

		if (((int) strlen(field->report_string) > field->props->width))
			field->props->width = (int) strlen(field->report_string);

		if ((rh->flags & RH_SORT_REQUIRED) &&
		    (field->props->flags & FLD_SORT_KEY)) {
			(*row->sort_fields)[field->props->sort_posn] = field;
		}
		dm_list_add(&row->fields, &field->list);
	}

	if (!(rh->flags & DM_REPORT_OUTPUT_BUFFERED))
		return dm_report_output(rh);

	return 1;
}

/*
 * Selection parsing
 */

/*
 * Other tokens (FIELD, VALUE, STRING, NUMBER, REGEX)
 *     FIELD := <strings of alphabet, number and '_'>
 *     VALUE := NUMBER | STRING
 *     REGEX := <strings quoted by '"', '\'', '(', '{', '[' or unquoted>
 *     NUMBER := <strings of [0-9]> (because sort_value is unsigned)
 *     STRING := <strings quoted by '"', '\'' or unquoted>
 */

static const char * _skip_space(const char *s)
{
	while (*s && isspace(*s))
		s++;
	return s;
}

static int _tok_op(struct op_def *t, const char *s, const char **end,
		   uint32_t expect)
{
	size_t len;

	s = _skip_space(s);

	for (; t->string; t++) {
		if (expect && !(t->flags & expect))
			continue;

		len = strlen(t->string);
		if (!strncmp(s, t->string, len)) {
			if (end)
				*end = s + len;
			return t->flags;
		}
	}

	if (end)
		*end = s;
	return 0;
}

static inline int _tok_op_log(const char *s, const char **end, uint32_t expect)
{
	return _tok_op(_op_log, s, end, expect);
}

static inline int _tok_op_cmp(const char *s, const char **end)
{
	return _tok_op(_op_cmp, s, end, 0);
}

 /*
  *
  * Input:
  *   s             - a pointer to the parsed string
  * Output:
  *   begin         - a pointer to the beginning of the token
  *   end           - a pointer to the end of the token + 1
  *                   or undefined if return value is NULL
  *   return value  - a starting point of the next parsing or
  *                   NULL if 's' doesn't match with token type
  *                   (the parsing should be terminated)
  */
static const char *_tok_value_number(const char *s,
				     const char **begin, const char **end)

{
	int is_float = 0;

	*begin = s;
	while (*s && ((!is_float && *s=='.' && (is_float=1)) || isdigit(*s)))
		s++;
	*end = s;

	if (*begin == *end)
		return NULL;

	return s;
}

/*
 * Input:
 *   s               - a pointer to the parsed string
 *   endchar         - terminating character
 *   end_op_flags    - terminating operator flags (see _op_log)
 *                     (if endchar is non-zero then endflags is ignored)
 * Output:
 *   begin           - a pointer to the beginning of the token
 *   end             - a pointer to the end of the token + 1
 *   end_op_flag_hit - the flag from endflags hit during parsing
 *   return value    - a starting point of the next parsing
 */
static const char *_tok_value_string(const char *s,
				     const char **begin, const char **end,
				     const char endchar, uint32_t end_op_flags,
				     uint32_t *end_op_flag_hit)
{
	uint32_t flag_hit = 0;

	*begin = s;

	/*
	 * If endchar is defined, scan the string till
	 * the endchar or the end of string is hit.
	 * This is in case the string is quoted and we
	 * know exact character that is the stopper.
	 */
	if (endchar) {
		while (*s && *s != endchar)
			s++;
		if (*s != endchar) {
			log_error("Missing end quote.");
			return NULL;
		}
		*end = s;
		s++;
	} else {
		/*
		 * If endchar is not defined then endchar is/are the
		 * operator/s as defined by 'endflags' arg or space char.
		 * This is in case the string is not quoted and
		 * we don't know which character is the exact stopper.
		 */
		while (*s) {
			if ((flag_hit = _tok_op(_op_log, s, NULL, end_op_flags)) || *s == ' ')
				break;
			s++;
		}
		*end = s;
		/*
		 * If we hit one of the strings as defined by 'endflags'
		 * and if 'endflag_hit' arg is provided, save the exact
		 * string flag that was hit.
		 */
		if (end_op_flag_hit)
			*end_op_flag_hit = flag_hit;
	}

	return s;
}

/*
 * Input:
 *   ft              - field type for which the value is parsed
 *   s               - a pointer to the parsed string
 * Output:
 *   begin           - a pointer to the beginning of the token
 *   end             - a pointer to the end of the token + 1
 *   flags           - parsing flags
 */
static const char *_tok_value_regex(const struct dm_report_field_type *ft,
				    const char *s, const char **begin,
				    const char **end, uint32_t *flags)
{
	char c;

	s = _skip_space(s);

	if (!*s) {
		log_error("Regular expression expected for selection field %s", ft->id);
		return NULL;
	}

	switch (*s) {
		case '(': c = ')'; break;
		case '{': c = '}'; break;
		case '[': c = ']'; break;
		case '"':
		case '\'': c = *s; break;
		default:  c = 0;
	}

	if (!(s = _tok_value_string(c ? s + 1 : s, begin, end, c, SEL_AND | SEL_OR | SEL_PRECEDENCE_PE, NULL))) {
		log_error("Failed to parse regex value for selection field %s.", ft->id);
		return NULL;
	}

	*flags |= DM_REPORT_FIELD_TYPE_STRING;
	return s;
}

/*
 * Input:
 *   ft              - field type for which the value is parsed
 *   s               - a pointer to the parsed string
 *   mem             - memory pool to allocate from
 * Output:
 *   begin           - a pointer to the beginning of the token
 *   end             - a pointer to the end of the token + 1
 *   flags           - parsing flags
 *   custom          - custom data specific to token type
 *                     (e.g. size unit factor)
 */
static const char *_tok_value(const struct dm_report_field_type *ft,
			      const char *s, const char **begin,
			      const char **end, uint32_t *flags,
			      struct dm_pool *mem, void *custom)
{
	int expected_type = ft->flags & DM_REPORT_FIELD_TYPE_MASK;
	uint64_t *factor;
	const char *tmp;
	char c = 0;

	s = _skip_space(s);

	switch (expected_type) {

		case DM_REPORT_FIELD_TYPE_STRING:
			if (*s == '"' || *s == '\'') {
				c = *s;
				s++;
			}
			if (!(s = _tok_value_string(s, begin, end, c, SEL_AND | SEL_OR | SEL_PRECEDENCE_PE, NULL))) {
				log_error("Failed to parse string value "
					  "for selection field %s.", ft->id);
				return NULL;
			}
			*flags |= DM_REPORT_FIELD_TYPE_STRING;
			break;

		case DM_REPORT_FIELD_TYPE_NUMBER:
		case DM_REPORT_FIELD_TYPE_SIZE:
			if (!(s = _tok_value_number(s, begin, end))) {
				log_error("Failed to parse numeric value "
					  "for selection field %s.", ft->id);
				return NULL;
			}
			factor = (uint64_t *) custom;
			*factor = dm_units_to_factor(s, &c, 0, &tmp);

			if (expected_type == DM_REPORT_FIELD_TYPE_NUMBER) {
				if (factor) {
					log_error("Found size unit specifier but "
						  "only numeric value expected for "
						  "selection field %s.",ft->id);
					return NULL;
				}
				*flags |= DM_REPORT_FIELD_TYPE_NUMBER;
			} else {
				s = tmp;
				*flags |= DM_REPORT_FIELD_TYPE_SIZE;
			}
	}

	return s;
}

/*
 * Input:
 *   s               - a pointer to the parsed string
 * Output:
 *   begin           - a pointer to the beginning of the token
 *   end             - a pointer to the end of the token + 1
 */
static const char *_tok_field_name(const char *s,
				    const char **begin, const char **end)
{
	char c;
	s = _skip_space(s);

	*begin = s;
	while ((c = *s) &&
	       (isalnum(c) || c == '_' || c == '-'))
		s++;
	*end = s;

	if (*begin == *end)
		return NULL;

	return s;
}



/*
 * Print row of headings
 */
static int _report_headings(struct dm_report *rh)
{
	struct field_properties *fp;
	const char *heading;
	char *buf = NULL;
	size_t buf_size = 0;

	if (rh->flags & RH_HEADINGS_PRINTED)
		return 1;

	rh->flags |= RH_HEADINGS_PRINTED;

	if (!(rh->flags & DM_REPORT_OUTPUT_HEADINGS))
		return 1;

	if (!dm_pool_begin_object(rh->mem, 128)) {
		log_error("dm_report: "
			  "dm_pool_begin_object failed for headings");
		return 0;
	}

	dm_list_iterate_items(fp, &rh->field_props) {
		if ((int) buf_size < fp->width)
			buf_size = (size_t) fp->width;
	}
	/* Including trailing '\0'! */
	buf_size++;

	if (!(buf = dm_malloc(buf_size))) {
		log_error("dm_report: Could not allocate memory for heading buffer.");
		goto bad;
	}

	/* First heading line */
	dm_list_iterate_items(fp, &rh->field_props) {
		if (fp->flags & FLD_HIDDEN)
			continue;

		heading = rh->fields[fp->field_num].heading;
		if (rh->flags & DM_REPORT_OUTPUT_ALIGNED) {
			if (dm_snprintf(buf, buf_size, "%-*.*s",
					 fp->width, fp->width, heading) < 0) {
				log_error("dm_report: snprintf heading failed");
				goto bad;
			}
			if (!dm_pool_grow_object(rh->mem, buf, fp->width)) {
				log_error("dm_report: Failed to generate report headings for printing");
				goto bad;
			}
		} else if (!dm_pool_grow_object(rh->mem, heading, 0)) {
			log_error("dm_report: Failed to generate report headings for printing");
			goto bad;
		}

		if (!dm_list_end(&rh->field_props, &fp->list))
			if (!dm_pool_grow_object(rh->mem, rh->separator, 0)) {
				log_error("dm_report: Failed to generate report headings for printing");
				goto bad;
			}
	}
	if (!dm_pool_grow_object(rh->mem, "\0", 1)) {
		log_error("dm_report: Failed to generate report headings for printing");
		goto bad;
	}
	log_print("%s", (char *) dm_pool_end_object(rh->mem));

	dm_free(buf);

	return 1;

      bad:
	dm_free(buf);
	dm_pool_abandon_object(rh->mem);
	return 0;
}

/*
 * Sort rows of data
 */
static int _row_compare(const void *a, const void *b)
{
	const struct row *rowa = *(const struct row * const *) a;
	const struct row *rowb = *(const struct row * const *) b;
	const struct dm_report_field *sfa, *sfb;
	uint32_t cnt;

	for (cnt = 0; cnt < rowa->rh->keys_count; cnt++) {
		sfa = (*rowa->sort_fields)[cnt];
		sfb = (*rowb->sort_fields)[cnt];
		if ((sfa->props->flags & DM_REPORT_FIELD_TYPE_NUMBER) ||
		    (sfa->props->flags & DM_REPORT_FIELD_TYPE_SIZE)) {
			const uint64_t numa =
			    *(const uint64_t *) sfa->sort_value;
			const uint64_t numb =
			    *(const uint64_t *) sfb->sort_value;

			if (numa == numb)
				continue;

			if (sfa->props->flags & FLD_ASCENDING) {
				return (numa > numb) ? 1 : -1;
			} else {	/* FLD_DESCENDING */
				return (numa < numb) ? 1 : -1;
			}
		} else {	/* DM_REPORT_FIELD_TYPE_STRING */
			const char *stra = (const char *) sfa->sort_value;
			const char *strb = (const char *) sfb->sort_value;
			int cmp = strcmp(stra, strb);

			if (!cmp)
				continue;

			if (sfa->props->flags & FLD_ASCENDING) {
				return (cmp > 0) ? 1 : -1;
			} else {	/* FLD_DESCENDING */
				return (cmp < 0) ? 1 : -1;
			}
		}
	}

	return 0;		/* Identical */
}

static int _sort_rows(struct dm_report *rh)
{
	struct row *(*rows)[];
	uint32_t count = 0;
	struct row *row;

	if (!(rows = dm_pool_alloc(rh->mem, sizeof(**rows) *
				dm_list_size(&rh->rows)))) {
		log_error("dm_report: sort array allocation failed");
		return 0;
	}

	dm_list_iterate_items(row, &rh->rows)
		(*rows)[count++] = row;

	qsort(rows, count, sizeof(**rows), _row_compare);

	dm_list_init(&rh->rows);
	while (count--)
		dm_list_add_h(&rh->rows, &(*rows)[count]->list);

	return 1;
}

/*
 * Produce report output
 */
static int _output_field(struct dm_report *rh, struct dm_report_field *field)
{
	char *field_id;
	int32_t width;
	uint32_t align;
	const char *repstr;
	char *buf = NULL;
	size_t buf_size = 0;

	if (rh->flags & DM_REPORT_OUTPUT_FIELD_NAME_PREFIX) {
		if (!(field_id = dm_strdup(rh->fields[field->props->field_num].id))) {
			log_error("dm_report: Failed to copy field name");
			return 0;
		}

		if (!dm_pool_grow_object(rh->mem, rh->output_field_name_prefix, 0)) {
			log_error("dm_report: Unable to extend output line");
			dm_free(field_id);
			return 0;
		}

		if (!dm_pool_grow_object(rh->mem, _toupperstr(field_id), 0)) {
			log_error("dm_report: Unable to extend output line");
			dm_free(field_id);
			return 0;
		}

		dm_free(field_id);

		if (!dm_pool_grow_object(rh->mem, "=", 1)) {
			log_error("dm_report: Unable to extend output line");
			return 0;
		}

		if (!(rh->flags & DM_REPORT_OUTPUT_FIELD_UNQUOTED) &&
		    !dm_pool_grow_object(rh->mem, "\'", 1)) {
			log_error("dm_report: Unable to extend output line");
			return 0;
		}
	}

	repstr = field->report_string;
	width = field->props->width;
	if (!(rh->flags & DM_REPORT_OUTPUT_ALIGNED)) {
		if (!dm_pool_grow_object(rh->mem, repstr, 0)) {
			log_error("dm_report: Unable to extend output line");
			return 0;
		}
	} else {
		if (!(align = field->props->flags & DM_REPORT_FIELD_ALIGN_MASK))
			align = ((field->props->flags & DM_REPORT_FIELD_TYPE_NUMBER) ||
				 (field->props->flags & DM_REPORT_FIELD_TYPE_SIZE)) ? 
				DM_REPORT_FIELD_ALIGN_RIGHT : DM_REPORT_FIELD_ALIGN_LEFT;

		/* Including trailing '\0'! */
		buf_size = width + 1;
		if (!(buf = dm_malloc(buf_size))) {
			log_error("dm_report: Could not allocate memory for output line buffer.");
			return 0;
		}

		if (align & DM_REPORT_FIELD_ALIGN_LEFT) {
			if (dm_snprintf(buf, buf_size, "%-*.*s",
					 width, width, repstr) < 0) {
				log_error("dm_report: left-aligned snprintf() failed");
				goto bad;
			}
			if (!dm_pool_grow_object(rh->mem, buf, width)) {
				log_error("dm_report: Unable to extend output line");
				goto bad;
			}
		} else if (align & DM_REPORT_FIELD_ALIGN_RIGHT) {
			if (dm_snprintf(buf, buf_size, "%*.*s",
					 width, width, repstr) < 0) {
				log_error("dm_report: right-aligned snprintf() failed");
				goto bad;
			}
			if (!dm_pool_grow_object(rh->mem, buf, width)) {
				log_error("dm_report: Unable to extend output line");
				goto bad;
			}
		}
	}

	if ((rh->flags & DM_REPORT_OUTPUT_FIELD_NAME_PREFIX) &&
	    !(rh->flags & DM_REPORT_OUTPUT_FIELD_UNQUOTED))
		if (!dm_pool_grow_object(rh->mem, "\'", 1)) {
			log_error("dm_report: Unable to extend output line");
			goto bad;
		}

	dm_free(buf);
	return 1;

bad:
	dm_free(buf);
	return 0;
}

static int _output_as_rows(struct dm_report *rh)
{
	struct field_properties *fp;
	struct dm_report_field *field;
	struct row *row;

	dm_list_iterate_items(fp, &rh->field_props) {
		if (fp->flags & FLD_HIDDEN) {
			dm_list_iterate_items(row, &rh->rows) {
				field = dm_list_item(dm_list_first(&row->fields), struct dm_report_field);
				dm_list_del(&field->list);
			}
			continue;
		}

		if (!dm_pool_begin_object(rh->mem, 512)) {
			log_error("dm_report: Unable to allocate output line");
			return 0;
		}

		if ((rh->flags & DM_REPORT_OUTPUT_HEADINGS)) {
			if (!dm_pool_grow_object(rh->mem, rh->fields[fp->field_num].heading, 0)) {
				log_error("dm_report: Failed to extend row for field name");
				goto bad;
			}
			if (!dm_pool_grow_object(rh->mem, rh->separator, 0)) {
				log_error("dm_report: Failed to extend row with separator");
				goto bad;
			}
		}

		dm_list_iterate_items(row, &rh->rows) {
			if ((field = dm_list_item(dm_list_first(&row->fields), struct dm_report_field))) {
				if (!_output_field(rh, field))
					goto bad;
				dm_list_del(&field->list);
			}

			if (!dm_list_end(&rh->rows, &row->list))
				if (!dm_pool_grow_object(rh->mem, rh->separator, 0)) {
					log_error("dm_report: Unable to extend output line");
					goto bad;
				}
		}

		if (!dm_pool_grow_object(rh->mem, "\0", 1)) {
			log_error("dm_report: Failed to terminate row");
			goto bad;
		}
		log_print("%s", (char *) dm_pool_end_object(rh->mem));
	}

	return 1;

      bad:
	dm_pool_abandon_object(rh->mem);
	return 0;
}

static int _output_as_columns(struct dm_report *rh)
{
	struct dm_list *fh, *rowh, *ftmp, *rtmp;
	struct row *row = NULL;
	struct dm_report_field *field;

	/* If headings not printed yet, calculate field widths and print them */
	if (!(rh->flags & RH_HEADINGS_PRINTED))
		_report_headings(rh);

	/* Print and clear buffer */
	dm_list_iterate_safe(rowh, rtmp, &rh->rows) {
		if (!dm_pool_begin_object(rh->mem, 512)) {
			log_error("dm_report: Unable to allocate output line");
			return 0;
		}
		row = dm_list_item(rowh, struct row);
		dm_list_iterate_safe(fh, ftmp, &row->fields) {
			field = dm_list_item(fh, struct dm_report_field);
			if (field->props->flags & FLD_HIDDEN)
				continue;

			if (!_output_field(rh, field))
				goto bad;

			if (!dm_list_end(&row->fields, fh))
				if (!dm_pool_grow_object(rh->mem, rh->separator, 0)) {
					log_error("dm_report: Unable to extend output line");
					goto bad;
				}

			dm_list_del(&field->list);
		}
		if (!dm_pool_grow_object(rh->mem, "\0", 1)) {
			log_error("dm_report: Unable to terminate output line");
			goto bad;
		}
		log_print("%s", (char *) dm_pool_end_object(rh->mem));
		dm_list_del(&row->list);
	}

	if (row)
		dm_pool_free(rh->mem, row);

	return 1;

      bad:
	dm_pool_abandon_object(rh->mem);
	return 0;
}

int dm_report_output(struct dm_report *rh)
{
	if (dm_list_empty(&rh->rows))
		return 1;

	if ((rh->flags & RH_SORT_REQUIRED))
		_sort_rows(rh);

	if ((rh->flags & DM_REPORT_OUTPUT_COLUMNS_AS_ROWS))
		return _output_as_rows(rh);
	else
		return _output_as_columns(rh);
}
