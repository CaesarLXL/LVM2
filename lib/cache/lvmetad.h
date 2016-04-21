/*
 * Copyright (C) 2012 Red Hat, Inc.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef _LVM_METAD_H
#define _LVM_METAD_H

#include "config-util.h"

struct volume_group;
struct cmd_context;
struct dm_config_tree;
enum activation_change;

typedef int (*activation_handler) (struct cmd_context *cmd,
				   const char *vgname, const char *vgid,
				   int partial, int changed,
				   enum activation_change activate);

#ifdef LVMETAD_SUPPORT

/*
 * lvmetad_connect: connect to lvmetad
 * lvmetad_disconnect: disconnect from lvmetad
 * lvmetad_make_unused: disconnect from lvmetad and refresh cmd filter
 * lvmetad_used: check if lvmetad is being used (i.e. is connected)
 */
int lvmetad_connect(struct cmd_context *cmd);
void lvmetad_disconnect(void);
void lvmetad_make_unused(struct cmd_context *cmd);
int lvmetad_used(void);


/*
 * Configure the socket that lvmetad_init will use to connect to the daemon.
 */
void lvmetad_set_socket(const char *);

/*
 * Check if lvmetad socket is present (either the one set by lvmetad_set_socket
 * or the default one if not set).
 */
int lvmetad_socket_present(void);

/*
 * Check if lvmetad pidfile is present, indicating that the lvmetad
 * process is running or not.
 */
int lvmetad_pidfile_present(void);

/*
 * Set the "lvmetad validity token" (currently only consists of the lvmetad
 * filter. See lvm.conf.
 */
void lvmetad_set_token(const struct dm_config_value *filter);

/*
 * Release allocated token.
 */
void lvmetad_release_token(void);

// FIXME What's described here doesn't appear to be implemented yet.
/*
 * Send a new version of VG metadata to lvmetad. This is normally called after
 * vg_write but before vg_commit. After vg_commit, lvmetad_vg_commit is called
 * to seal the transaction. The result of lvmetad_vg_update is that the new
 * metadata is stored tentatively in lvmetad, but it is not used until
 * lvmetad_vg_commit. The request is validated immediately and lvmetad_vg_commit
 * only constitutes a pointer update.
 */
int lvmetad_vg_update(struct volume_group *vg);

/*
 * Inform lvmetad that a VG has been removed. This is not entirely safe, but is
 * only needed during vgremove, which does not wipe PV labels and therefore
 * cannot mark the PVs as gone.
 */
int lvmetad_vg_remove(struct volume_group *vg);

/*
 * Notify lvmetad that a PV has been found. It is not an error if the PV is
 * already marked as present in lvmetad. If a non-NULL vg pointer is supplied,
 * it is taken to represent the metadata read from the MDA(s) present on that
 * PV. It *is* an error if: the VG is already known to lvmetad, the sequence
 * number on the cached and on the discovered PV match but the metadata content
 * does not.
 */
int lvmetad_pv_found(const struct id *pvid, struct device *dev,
		     const struct format_type *fmt, uint64_t label_sector,
		     struct volume_group *vg, activation_handler handler);

/*
 * Inform the daemon that the device no longer exists.
 */
int lvmetad_pv_gone(dev_t devno, const char *pv_name, activation_handler handler);
int lvmetad_pv_gone_by_dev(struct device *dev, activation_handler handler);

/*
 * Request a list of all PVs available to lvmetad. If requested, this will also
 * read labels off all the PVs to populate lvmcache.
 */
int lvmetad_pv_list_to_lvmcache(struct cmd_context *cmd);

/*
 * Lookup an individual PV.
 * If found is not NULL, it is set according to whether or not the PV is found,
 * otherwise if the PV is not found an error is returned.
 */
int lvmetad_pv_lookup(struct cmd_context *cmd, struct id pvid, int *found);
int lvmetad_pv_lookup_by_dev(struct cmd_context *cmd, struct device *dev, int *found);

/*
 * Request a list of all VGs available to lvmetad and use it to fill in
 * lvmcache..
 */
int lvmetad_vg_list_to_lvmcache(struct cmd_context *cmd);

/*
 * Request a list of vgid/vgname pairs for all VGs known to lvmetad.
 * Does not do vg_lookup's on each VG, and does not populate lvmcache.
 */
int lvmetad_get_vgnameids(struct cmd_context *cmd, struct dm_list *vgnameids);

/*
 * Find a VG by its ID or its name in the lvmetad cache. Gives NULL if the VG is
 * not found.
 */
struct volume_group *lvmetad_vg_lookup(struct cmd_context *cmd,
				       const char *vgname, const char *vgid);

/*
 * Scan a single device and update lvmetad with the result(s).
 */
int lvmetad_pvscan_single(struct cmd_context *cmd, struct device *dev,
			  activation_handler handler, int ignore_obsolete);

int lvmetad_pvscan_all_devs(struct cmd_context *cmd, activation_handler handler, int do_wait);
int lvmetad_pvscan_foreign_vgs(struct cmd_context *cmd, activation_handler handler);

int lvmetad_vg_clear_outdated_pvs(struct volume_group *vg);
void lvmetad_validate_global_cache(struct cmd_context *cmd, int force);
int lvmetad_token_matches(struct cmd_context *cmd);

int lvmetad_vg_is_foreign(struct cmd_context *cmd, const char *vgname, const char *vgid);

int lvmetad_is_disabled(struct cmd_context *cmd, const char **reason);
void lvmetad_set_disabled(struct cmd_context *cmd, const char *reason);
void lvmetad_clear_disabled(struct cmd_context *cmd);

#  else		/* LVMETAD_SUPPORT */

#    define lvmetad_disconnect()	do { } while (0)
#    define lvmetad_connect(cmd)	(0)
#    define lvmetad_make_unused(cmd)	do { } while (0)
#    define lvmetad_used()		(0)
#    define lvmetad_set_socket(a)	do { } while (0)
#    define lvmetad_socket_present()	(0)
#    define lvmetad_pidfile_present()   (0)
#    define lvmetad_set_token(a)	do { } while (0)
#    define lvmetad_release_token()	do { } while (0)
#    define lvmetad_vg_update(vg)	(1)
#    define lvmetad_vg_remove(vg)	(1)
#    define lvmetad_pv_found(pvid, dev, fmt, label_sector, vg, handler)	(1)
#    define lvmetad_pv_gone(devno, pv_name, handler)	(1)
#    define lvmetad_pv_gone_by_dev(dev, handler)	(1)
#    define lvmetad_pv_list_to_lvmcache(cmd)	(1)
#    define lvmetad_pv_lookup(cmd, pvid, found)	(0)
#    define lvmetad_pv_lookup_by_dev(cmd, dev, found)	(0)
#    define lvmetad_vg_list_to_lvmcache(cmd)	(1)
#    define lvmetad_get_vgnameids(cmd, vgnameids)       do { } while (0)
#    define lvmetad_vg_lookup(cmd, vgname, vgid)	(NULL)
#    define lvmetad_pvscan_single(cmd, dev, handler, ignore_obsolete)	(0)
#    define lvmetad_pvscan_all_devs(cmd, handler, do_wait)	(0)
#    define lvmetad_pvscan_foreign_vgs(cmd, handler)	(0)
#    define lvmetad_vg_clear_outdated_pvs(vg)           do { } while (0)
#    define lvmetad_validate_global_cache(cmd, force)	do { } while (0)
#    define lvmetad_vg_is_foreign(cmd, vgname, vgid) (0)
#    define lvmetad_token_matches(cmd) (1)
#    define lvmetad_is_disabled(cmd, reason) (0)
#    define lvmetad_set_disabled(cmd, reason) do { } while (0)
#    define lvmetad_clear_disabled(cmd) do { } while (0)

#  endif	/* LVMETAD_SUPPORT */

#endif
