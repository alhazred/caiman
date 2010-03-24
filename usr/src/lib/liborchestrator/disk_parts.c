/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2010 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/types.h>

#include "orchestrator_private.h"
#ifdef	__sparc
#define	fdisk_is_dos_extended(p) (B_FALSE)
#else
#include <libfdisk.h>
#endif

/*
 * fdisk(1m) rsect - must be space before logical partition
 */
#define	LOGICAL_PARTITION_PAD (63)

/*
 * shorthand method for finding end of ith partition
 */
#define	PARTITION_END(i) \
	(sorted_parts[(i)].partition_offset_sec + \
	sorted_parts[(i)].partition_size_sec)

static struct free_region {
	uint64_t free_offset;
	uint64_t free_size;
};

boolean_t	whole_disk = B_FALSE; /* assume existing partition */

static void mark_for_deletion_by_index(int);
static void delete_all_logical_partitions(void);

/* free space management */
static struct free_region free_space_table[OM_NUMPART + 1];
static int n_fragments = 0;
static struct {
	int		partition_id;
	uint64_t	partition_offset_sec;
	uint64_t	partition_size_sec;
} sorted_parts[OM_NUMPART];
static int n_sorted_parts = 0;
/*
 * sorted_parts and n_sorted_parts are used only for a temporary sort of the
 * partitions according to starting offset when building the free space table
 */
static struct free_region *find_unused_region_of_size(uint64_t, boolean_t);
static void insertion_sort_partition_info(partition_info_t *);
static void sort_used_regions(boolean_t);
static boolean_t build_free_space_table(boolean_t);
static void append_free_space_table(uint64_t, uint64_t);
static struct free_region *find_largest_free_region(boolean_t);
static struct free_region *find_free_region_best_fit(uint64_t, boolean_t);

/* logging */
static void log_partition_map(void);
static void log_used_regions(void);
static void log_free_space_table(void);

static partition_info_t *get_extended_partition_info(disk_parts_t *);

/* ----------------- definition of private functions ----------------- */

/*
 * is_resized_partition
 * This function checks if partition size changed
 *
 * Input:	pold, pnew - pointer to the old and new partition entries
 *
 * Return:	B_TRUE - partition was resized
 *		B_FALSE - partition size not changed
 */

static boolean_t
is_resized_partition(partition_info_t *pold, partition_info_t *pnew)
{
	return (pold->partition_size != pnew->partition_size);
}


/*
 * is_changed_partition
 * This function checks if partition changed. It means that
 * [1] size changed OR
 * [2] type changed for used partition (size is not 0)
 *
 * Input:	pold, pnew - pointer to the old and new partition entries
 *
 * Return:	B_TRUE - partition was resized
 *		B_FALSE - partition size not changed
 */

static boolean_t
is_changed_partition(partition_info_t *pold, partition_info_t *pnew)
{
	return (is_resized_partition(pold, pnew) ||
	    (pold->partition_type != pnew->partition_type &&
	    pnew->partition_size != 0));
}


/*
 * is_deleted_partition
 * This function checks if partition was deleted
 *
 * Input:	pold, pnew - pointer to the old and new partition entries
 *
 * Return:	B_TRUE - partition was deleted
 *		B_FALSE - partition was not deleted
 */

static boolean_t
is_deleted_partition(partition_info_t *pold, partition_info_t *pnew)
{
	return (pold->partition_size != 0 && pnew->partition_size == 0);
}


/*
 * is_created_partition
 * This function checks if partition was created
 *
 * Input:	pold, pnew - pointer to the old and new partition entries
 *
 * Return:	B_TRUE - partition was created
 *		B_FALSE - partition already existed
 */

static boolean_t
is_created_partition(partition_info_t *pold, partition_info_t *pnew)
{
	return (pold->partition_size == 0 && pnew->partition_size != 0);
}


/*
 * is_used_partition
 * This function checks if partition_info_t structure
 * describes used partition entry.
 *
 * Entry is considered to be used if
 * [1] partition size is greater than 0
 * [2] type is not set to unused - id 0 or 100
 *
 * Input:	pentry - pointer to the partition entry
 *
 * Return:	B_TRUE - partition entry is in use
 *		B_FALSE - partition entry is empty
 */

boolean_t
is_used_partition(partition_info_t *pentry)
{
	return (pentry->partition_type != 0 &&
	    pentry->partition_type != UNUSED);
}

/*
 * partition_index_by_id
 * This function checks if partition number (1-4) is in use
 *
 * Input:	ipno - partition number
 *
 * Return:	index to partition table (0-3)
 *		-1 not found or not in use
 */

static int
partition_index_by_id(int ipno)
{
	partition_info_t *pinfo;
	int ipart;

	assert(committed_disk_target != NULL);
	assert(committed_disk_target->dparts != NULL);

	pinfo = committed_disk_target->dparts->pinfo;
	for (ipart = 0; ipart < OM_NUMPART; ipart++, pinfo++) {
		if (pinfo->partition_id == ipno && is_used_partition(pinfo))
			return (ipart);
	}
	return (-1);
}

/*
 * set_partition_unused
 * This function sets partition entry as unused
 *
 * Input:	pentry - pointer to the partition entry
 */

static void
set_partition_unused(partition_info_t *pentry)
{
	bzero(pentry, sizeof (*pentry));
	pentry->partition_type = UNUSED;
	pentry->partition_size = 0;
	pentry->partition_size_sec = 0;
}

/*
 * get_first_used_partition
 * This function will search array of partition_info_t structures
 * and will find the first used entry
 *
 * see is_used_partition() for how empty partition is defined
 *
 * Input:	pentry - pointer to the array of partition entries
 *
 * Output:	None
 *
 * Return:	>=0	- index of first used partition entry
 *		-1	- array contains only empty entries
 */

static int
get_first_used_partition(partition_info_t *pentry)
{
	int i;

	for (i = 0; i < OM_NUMPART; i++) {
		if (is_used_partition(&pentry[i]))
			return (i);
	}

	return (-1);
}

/*
 * get_last_used_partition
 * This function will search array of partition_info_t structures
 * and will find the last used entry
 *
 * see is_used_partition() for how empty partition is defined
 *
 * Input:	pentry - pointer to the array of partition entries
 *
 * Output:	None
 *
 * Return:	>=0	- index of first used partition entry
 *		-1	- array contains only empty entries
 */

static int
get_last_used_partition(partition_info_t *pentry, int index)
{
	int i;
	int highest = -1;
	int ret = -1;

	if (IS_LOG_PAR(index + 1)) {
		for (i = OM_NUMPART - 1; i >= FD_NUMPART; i--) {
			if (is_used_partition(&pentry[i]) &&
			    pentry[i].partition_order > highest) {
				ret = i;
				highest = pentry[i].partition_order;
			}
		}
	} else {
		for (i = FD_NUMPART - 1; i >= 0; i--) {
			if (is_used_partition(&pentry[i]) &&
			    pentry[i].partition_order > highest) {
				ret = i;
				highest = pentry[i].partition_order;
			}
		}
	}

	return (ret);
}

/*
 * get_next_used_partition
 * This function will search array of partition_info_t structures
 * and will find the next used entry
 *
 * Input:	pentry - pointer to the array of partition entries
 *		current - current index
 *
 * Output:	None
 *
 * Return:	>=0	- index of next used partition entry
 *		-1	- no more used entries
 */

static int
get_next_used_partition(partition_info_t *pentry, int current)
{
	int i;
	int next_order;

	/* Starting at current partition_order + 1 */
	/* cycle through all partitions to find */
	/* first used partition after this one */
	for (next_order = pentry[current].partition_order + 1;
	    next_order <= OM_NUMPART;
	    next_order++) {
		for (i = 0; i < OM_NUMPART; i++) {
			if (is_used_partition(&pentry[i]) &&
			    pentry[i].partition_order == next_order) {
				return (i);
			}
		}
	}

	return (-1);
}


/*
 * get_previous_used_partition
 * This function will search array of partition_info_t structures
 * and will find the previous used entry in terms of order on disk
 *
 * Input:	pentry - pointer to the array of partition entries
 *		current - current index
 *
 * Output:	None
 *
 * Return:	>=0	- index of next used partition entry
 *		-1	- no more used entries
 */

static int
get_previous_used_partition(partition_info_t *pentry, int current)
{
	int i;

	assert(pentry[current].partition_order > 1);

	if (IS_LOG_PAR(current + 1)) {
		for (i = FD_NUMPART; i < OM_NUMPART; i++)
			if (pentry[current].partition_order - 1 ==
			    pentry[i].partition_order)
				return (i);
	} else {
		for (i = 0; i < FD_NUMPART; i++)
			if (pentry[current].partition_order - 1 ==
			    pentry[i].partition_order)
				return (i);
	}
	return (-1);
}


/*
 * is_first_used_partition
 * This function checks if index points
 * to the first used partition entry.
 *
 * Input:	pentry	- pointer to the array of partition entries
 *		index	- index of partition to be checked
 *
 * Return:	B_TRUE - partition entry is in use
 *		B_FALSE - partition entry is empty
 */

static boolean_t
is_first_used_partition(partition_info_t *pentry, int index)
{
	return (pentry[index].partition_order ==
	    (IS_LOG_PAR(index + 1) ? FD_NUMPART + 1 : 1));
}

/*
 * is_last_used_partition
 * This function checks if index points to
 * the last used partition entry.
 *
 * Input:	pentry	- pointer to the array of partition entries
 *		index	- index of partition to be checked
 *
 * Return:	B_TRUE - partition entry is in use
 *		B_FALSE - partition entry is empty
 */

static boolean_t
is_last_used_partition(partition_info_t *pentry, int index)
{
	return (get_last_used_partition(pentry, index) == index ?
	    B_TRUE : B_FALSE);
}

static void
log_partition_map()
{
	int ipar;
	partition_info_t *pinfo;

	pinfo = committed_disk_target->dparts->pinfo;
	om_debug_print(OM_DBGLVL_INFO,
	    "id\ttype\torder\tsector offset\tsize in sectors\n");
	for (ipar = 0; ipar < OM_NUMPART; ipar++) {
		om_debug_print(OM_DBGLVL_INFO, "%d\t%02X\t%2d\t%lld\t%lld\n",
		    pinfo[ipar].partition_id,
		    pinfo[ipar].partition_type,
		    pinfo[ipar].partition_order,
		    pinfo[ipar].partition_offset_sec,
		    pinfo[ipar].partition_size_sec);
	}
}

/*
 * from install target disk, find extended partition struct pointer
 * returns NULL if no extended partition defined
 */
partition_info_t *
get_extended_partition_info(disk_parts_t *ipinfo)
{
	int ipar;
	partition_info_t *pinfo;

	if (ipinfo == NULL) {
		if (committed_disk_target == NULL ||
		    committed_disk_target->dparts == NULL ||
		    committed_disk_target->dparts->pinfo == NULL)
			return (B_FALSE);
		/*
		 * extended partition could be of several different types
		 */
		pinfo = committed_disk_target->dparts->pinfo;
	} else
		pinfo = ipinfo->pinfo;
	for (ipar = 0; ipar < OM_NUMPART; ipar++, pinfo++)
		if (is_used_partition(pinfo) &&
		    fdisk_is_dos_extended(pinfo->partition_type))
			return (pinfo);
	return (NULL);	/* no extended partition found */
}

/*
 * resize partition using all critical struct elements
 */
static void
resize_partition(partition_info_t *p_new, int offset)
{
	/* push starting offset ahead */
	p_new->partition_offset_sec += offset;
	p_new->partition_offset += (offset/BLOCKS_TO_MB);
	/* subtract from region size */
	p_new->partition_size_sec -= offset;
	p_new->partition_size -= (offset/BLOCKS_TO_MB);
}

/*
 * adjust start of logical partition to make 63 unused sectors before it.
 *
 * p_new - pointer to list of partitions for the new configuration
 * offset - offset into p_new to represent partition to adjust
 *	assumed to be partition number - 1 per convention
 * extpinfo - partition information for the extended partition
 *
 * assumes that partition_order element is set to indicate order of partitions
 */
static void
logical_start_adjust(partition_info_t *p_new, int offset,
    partition_info_t *extpinfo)
{
	partition_info_t	*p_prev;
	int			previous;
	uint64_t		first_free_sector;
	int64_t			diff;

	if (offset < FD_NUMPART)
		return;	/* consider logical partitions only */

	if (is_first_used_partition(p_new, offset)) {
		/*
		 * start counting from start of extended partition
		 */
		assert(extpinfo != NULL);
		first_free_sector = extpinfo->partition_offset_sec;
	} else {
		/*
		 * start counting from end of previous partition
		 */
		previous = get_previous_used_partition(p_new, offset);
		assert(previous != -1);
		p_prev = &p_new[previous];

		first_free_sector =
		    p_prev->partition_offset_sec +
		    p_prev->partition_size_sec;
	}

	diff = p_new[offset].partition_offset_sec - first_free_sector;
	if (diff < LOGICAL_PARTITION_PAD) {
		int padsize = LOGICAL_PARTITION_PAD - diff;

		resize_partition(&p_new[offset], padsize);
		om_debug_print(OM_DBGLVL_INFO,
		    "Logical partition %d (%02X) starting sector moved forward "
		    "%ld sectors. (new starting sector %lld)\n",
		    offset + 1, p_new[offset].partition_type, padsize,
		    p_new[offset].partition_offset_sec);
	}
}

/*
 * given order in partition table and the table,
 * return index for partition
 */
static int
map_order_to_index(int order, partition_info_t *pentry)
{
	int i;

	for (i = 0; i < OM_NUMPART; i++)
		if (is_used_partition(&pentry[i]) &&
		    pentry[i].partition_order == order)
			return (i);
	return (-1);
}

/*
 * trim end of partition within allowable limits.
 *
 * given partition table, index into it, extended partition info,
 *	and disk target info:
 *	- primary - limit by disk size
 *	- logical - limit by extended partition size
 *
 * For a primary partition, the limit is the end of the disk
 * For a logical partition, the limit is the end of the extended partition
 */
static void
partition_end_adjust(partition_info_t *p_new, int offset,
    partition_info_t *extpinfo, disk_target_t *dt)
{
	uint64_t	first_sector_after;
	int64_t		diff;

	/*
	 * find offset of first sector beyond end of disk or extended partition.
	 * Trim last partition on disk/extended partition so that
	 * it doesn't exceed this limit
	 */
	first_sector_after = (offset < FD_NUMPART ? dt->dinfo.disk_size_sec :
	    extpinfo->partition_offset_sec + extpinfo->partition_size_sec);

	/*
	 * calculate difference between maximum and actual end
	 */
	diff = p_new->partition_offset_sec +
	    p_new->partition_size_sec - first_sector_after;

	if (diff <= 0)
		return;
	/*
	 * if overextended, trim size to fit
	 */
	p_new->partition_size_sec -= diff;
	om_set_part_mb_size_from_sec(p_new);
	om_debug_print(OM_DBGLVL_INFO,
	    "Partition %d (%02X) size trimmed by %lld sectors "
	    "(new size %lld)\n",
	    offset + 1, p_new->partition_type, diff,
	    p_new->partition_size_sec);
}

/*
 * from install target disk, find Solaris partition
 * returns B_TRUE if Solaris partition is in logical partition
 * returns B_FALSE if not or if Solaris partition was not found
 */
boolean_t
om_install_partition_is_logical()
{
	int ipar;
	partition_info_t *pinfo;

	if (committed_disk_target == NULL ||
	    committed_disk_target->dparts == NULL ||
	    committed_disk_target->dparts->pinfo == NULL)
		return (B_FALSE);
	/*
	 * If the partition is SOLARIS2 or SOLARIS and the content has
	 * been confirmed not to be Linux swap,
	 * if the fdisk partition number is > FD_NUMPART,
	 * the partition is a logical partition
	 */
	pinfo = committed_disk_target->dparts->pinfo;
	for (ipar = 0; ipar < OM_NUMPART; ipar++, pinfo++) {
		if (!is_used_partition(pinfo))
			continue;
		if (pinfo->partition_type == SUNIXOS2 ||
		    (pinfo->partition_type == SUNIXOS &&
		    pinfo->content_type != OM_CTYPE_LINUXSWAP))
			return (pinfo->partition_id > FD_NUMPART);
	}
	return (B_FALSE);	/* no Solaris partition found */
}

/* ----------------- definition of public functions ----------------- */

/*
 * om_get_disk_partition_info
 * This function will return the partition information of the specified disk.
 * Input:	om_handle_t handle - The handle returned by
 *		om_initiate_target_discovery()
 * 		char *diskname - The name of the disk
 * Output:	None
 * Return:	disk_parts_t * - The fdisk partition information for
 *		the disk with diskname will be 	returned. The space will be
 *		allocated here linked and returned to the caller.
 *		NULL - if the partition data can't be returned.
 */
/*ARGSUSED*/
disk_parts_t *
om_get_disk_partition_info(om_handle_t handle, char *diskname)
{
	disk_parts_t	*dp;

	/*
	 * Validate diskname
	 */
	if (diskname == NULL || diskname[0] == '\0') {
		om_set_error(OM_BAD_DISK_NAME);
		return (NULL);
	}

	/*
	 * If the discovery is not yet completed or not started,
	 * return error
	 */
	if (!disk_discovery_done) {
		om_set_error(OM_DISCOVERY_NEEDED);
		return (NULL);
	}

	if (system_disks == NULL) {
		om_set_error(OM_NO_DISKS_FOUND);
		return (NULL);
	}

	/*
	 * Find the disk from the cache using the diskname
	 */
	dp = find_partitions_by_disk(diskname);
	if (dp == NULL) {
		return (NULL);
	}

	/*
	 * copy the data
	 */
	return (om_duplicate_disk_partition_info(handle, dp));
}

/*
 * om_free_disk_partition_info
 * This function will free up the disk information data allocated during
 * om_get_disk_partition_info().
 * Input:	om_handle_t handle - The handle returned by
 *		om_initiate_target_discovery()
 *		disk_parts_t *dinfo - The pointer to disk_parts_t. Usually
 *		returned by om_get_disk_partition_info().
 * Output:	None.
 * Return:	None.
 */
/*ARGSUSED*/
void
om_free_disk_partition_info(om_handle_t handle, disk_parts_t *dpinfo)
{
	if (dpinfo == NULL) {
		return;
	}

	local_free_part_info(dpinfo);
}

/*
 * om_validate_and_resize_disk_partitions
 * This function will check whether the the partition information of the
 * specified disk is valid. If the reuqested space can't be allocated, then
 * suggested partition allocation will be returned.
 * Input:	disk_parts_t *dpart
 *		partition_allocation_scheme
 *			if not set for AI, assume allocation for GUI
 * Output:	None
 * Return:	disk_parts_t *, with the corrected values. If the sizes
 *		are valid, the return disk_parts_t structure will have same
 *		data as the the	disk_parts_t structure passed as the input.
 *		NULL, if the partition data is invalid.
 * Note:	If the partition is not valid, the om_errno will be set to
 *		the actual error condition. The error information can be
 *		obtained by calling om_get_errno().
 *
 *		This function checks:
 *		- Whether the total partition space is < disk space
 *		- There is enough space to create a new partition
 *		- for the GUI allocation, if there are holes between partitions,
 *		can the new partition fit into any of the holes?
 *		GUI_allocation should serve the use case of the original
 *		GUI in which only partition sizes are user-specified and
 *		starting offsets are unset.
 *		For GUI only, dpart element partition_order must contain
 *			order of the partitions by disk sector:
 *			- primary partitions are ordered by starting offset 1:4
 *			- logical partitions are ordered by starting offset 5:36
 *
 *		The first cylinder will be reserved for Solaris
 */
/*ARGSUSED*/
disk_parts_t *
om_validate_and_resize_disk_partitions(om_handle_t handle, disk_parts_t *dpart,
	partition_allocation_scheme_t partition_allocation_scheme)
{
	boolean_t	changed = B_FALSE;
	disk_target_t	*dt;
	disk_parts_t	*dp, *new_dp;
	int		i, j;
	partition_info_t *extpinfo;
	int		nparts;

	/*
	 * validate the input
	 */
	if (dpart == NULL) {
		om_set_error(OM_INVALID_DISK_PARTITION);
		return (NULL);
	}

	/*
	 * Is the target discovery completed?
	 */
	if (!disk_discovery_done) {
		om_set_error(OM_DISCOVERY_NEEDED);
		return (NULL);
	}

	if (system_disks  == NULL) {
		om_set_error(OM_NO_DISKS_FOUND);
		return (NULL);
	}

	if (dpart->disk_name == NULL) {
		om_set_error(OM_INVALID_DISK_PARTITION);
		return (NULL);
	}

	/*
	 * Find the disk from the cache using the diskname
	 */
	dt = find_disk_by_name(dpart->disk_name);
	if (dt == NULL) {
		om_set_error(OM_BAD_DISK_NAME);
		return (NULL);
	}

	/*
	 * Create the disk_parts_t structure to be returned
	 */
	new_dp = om_duplicate_disk_partition_info(handle, dpart);
	if (new_dp == NULL) {
		return (NULL);
	}

	/*
	 * find an extended partition if it exists
	 */
	extpinfo = get_extended_partition_info(new_dp);
	nparts = (extpinfo == NULL ? FD_NUMPART:OM_NUMPART);

	/*
	 * check if "whole disk" path was selected. It is true if
	 * both following conditions are met:
	 * [1] Only first partition is defined. Rest are left unused
	 *	(size ==0 &&, type == 100)
	 * [2] First partition is Solaris2 and occupies all available space
	 */

	whole_disk = B_TRUE;

	if ((new_dp->pinfo[0].partition_size != dt->dinfo.disk_size) ||
	    (new_dp->pinfo[0].partition_type != SUNIXOS2)) {
		om_debug_print(OM_DBGLVL_INFO, "entire disk not used\n");
		whole_disk = B_FALSE;
	}

	for (i = 1; i < nparts && whole_disk == B_TRUE; i++) {
		if ((new_dp->pinfo[i].partition_size != 0) ||
		    is_used_partition(&new_dp->pinfo[i])) {
			om_debug_print(OM_DBGLVL_INFO,
			    "Entire disk not used\n");
			whole_disk = B_FALSE;
		}
	}

	if (whole_disk) {
		return (new_dp);
	}

	/*
	 * if target disk is empty (there are no partitions defined),
	 * create dummy partition configuration. This allows using
	 * unified code for dealing with partition changes.
	 */

	if (dt->dparts == NULL) {
		om_log_print("disk currently doesn't contain any partition\n");

		dp = om_duplicate_disk_partition_info(handle, dpart);

		if (dp == NULL) {
			om_log_print("Couldn't duplicate partition info\n");
			return (NULL);
		}

		(void) memset(dp->pinfo, 0, sizeof (partition_info_t) *
		    OM_NUMPART);
	} else
		dp = dt->dparts;

	/*
	 * Compare the size and partition type (for used partition)
	 * of each partition to decide whether any of them was changed.
	 */

	for (i = 0; i < nparts; i++) {
		if (is_changed_partition(&dp->pinfo[i], &new_dp->pinfo[i])) {
			om_log_print("disk partition info changed\n");
			changed = B_TRUE;
			break;
		}
	}

	if (!changed) {
		/*
		 * No change in the partition table.
		 */
		om_log_print("disk partition info not changed\n");

		/* release partition info if allocated if this function */
		if (dt->dparts == NULL)
			local_free_part_info(dp);

		return (new_dp);
	}

	/*
	 * Finally calculate sector geometry information for changed
	 * partitions
	 */

	om_debug_print(OM_DBGLVL_INFO,
	    "Partition LBA information before recalculation\n");

	for (i = 0; i < nparts; i++) {
		om_debug_print(OM_DBGLVL_INFO,
		    "[%d] pid=%d ord=%d, type=%02X, beg=%llu(%lu MiB), "
		    "size=%llu(%lu MiB)\n", i,
		    new_dp->pinfo[i].partition_id,
		    new_dp->pinfo[i].partition_order,
		    new_dp->pinfo[i].partition_type,
		    new_dp->pinfo[i].partition_offset_sec,
		    new_dp->pinfo[i].partition_offset,
		    new_dp->pinfo[i].partition_size_sec,
		    new_dp->pinfo[i].partition_size);
	}

	for (j = 0; j < nparts; j++) {
		partition_info_t	*p_orig;
		partition_info_t	*p_new;

		if (partition_allocation_scheme == GUI_allocation) {
			/*
			 * for GUI, look at all partitions by order on disk
			 */
			i = map_order_to_index(j + 1, &new_dp->pinfo[0]);
			if (i == -1)
				continue;
			/*
			 * compare partitions by partition number
			 */
			p_new = &new_dp->pinfo[i];
			p_orig = &dp->pinfo[p_new->partition_id - 1];
		} else {
			/*
			 * for AI, look at all partitions by index
			 * regardless of order on disk
			 */
			i = j;
			p_orig = &dp->pinfo[i];
			p_new = &new_dp->pinfo[i];
		}

		if (p_orig->partition_type != 0 || p_new->partition_type != 0) {
			om_debug_print(OM_DBGLVL_INFO,
			    "examining orig partition [%d] pid=%d ord=%d, type"
			    "=%02X, beg=%llu(%lu MiB), size=%llu(%lu MiB)\n",
			    p_orig->partition_id - 1,
			    p_orig->partition_id,
			    p_orig->partition_order,
			    p_orig->partition_type,
			    p_orig->partition_offset_sec,
			    p_orig->partition_offset,
			    p_orig->partition_size_sec,
			    p_orig->partition_size);
			om_debug_print(OM_DBGLVL_INFO,
			    "examining new  partition [%d] pid=%d ord=%d, type"
			    "=%02X, beg=%llu(%lu MiB), size=%llu(%lu MiB)\n",
			    p_new->partition_id - 1,
			    p_new->partition_id,
			    p_new->partition_order,
			    p_new->partition_type,
			    p_new->partition_offset_sec,
			    p_new->partition_offset,
			    p_new->partition_size_sec,
			    p_new->partition_size);
		}

		/*
		 * If the partition was not resized, skip it, since
		 * other modifications (change of type) don't require
		 * offset & size recalculation
		 */

		if (!is_resized_partition(p_orig, p_new)) {
			/*
			 * retain existing data for later calculations
			 */
			om_debug_print(OM_DBGLVL_INFO,
			    "Partition pid=%d, ord=%d, type=%02X "
			    "was NOT resized\n",
			    p_orig->partition_id,
			    p_orig->partition_order,
			    p_orig->partition_type);
			p_new->partition_size_sec = p_orig->partition_size_sec;
			p_new->partition_offset_sec =
			    p_orig->partition_offset_sec;
			p_new->partition_offset = p_orig->partition_offset;
			continue;
		}

		om_debug_print(OM_DBGLVL_INFO,
		    "Partition pid=%d, ord=%d, type=%02X was resized: "
		    "old = %d, new = %d\n",
		    p_orig->partition_id,
		    p_orig->partition_order,
		    p_orig->partition_type,
		    p_orig->partition_size,
		    p_new->partition_size);

		/*
		 * If partition is deleted (marked as "UNUSED"),
		 * clear offset and size
		 */

		if (is_deleted_partition(p_orig, p_new)) {
			om_debug_print(OM_DBGLVL_INFO,
			    "Partition pid=%d, ord=%s, type=%02X "
			    "is to be deleted\n",
			    p_orig->partition_id,
			    p_orig->partition_order,
			    p_orig->partition_type);

			p_new->partition_offset = 0;
			p_new->partition_offset_sec =
			    p_new->partition_size_sec = 0;

			/*
			 * don't clear partition_id - it is "read only"
			 * from orchestrator point of view - modified by GUI
			 */
			continue;
		}

		if (is_created_partition(p_orig, p_new)) {
			om_debug_print(OM_DBGLVL_INFO,
			    "Partition pid=%d, ord=%d, type=%02X "
			    "is to be created\n",
			    p_new->partition_id,
			    p_new->partition_order,
			    p_new->partition_type);

			if (partition_allocation_scheme != GUI_allocation)
				om_debug_print(OM_DBGLVL_INFO,
				    "Partition offset=%lld, "
				    "size=%lld is created\n",
				    p_new->partition_offset_sec,
				    p_new->partition_size_sec);
		}

		if (partition_allocation_scheme == GUI_allocation) {
			/*
			 * Assume initial GUI support
			 *
			 * Calculate sector offset information
			 *
			 * Gaps are not allowed for now - partition starts
			 * right after previous used partition
			 *
			 * If this is the first partition, it starts at the
			 * second cylinder - adjust size accordingly
			 */

			if (is_first_used_partition(new_dp->pinfo, i)) {
				/*
				 * place first partition as close to the front
				 * as possible while allowing the 1st cylinder
				 * to be used for the boot partition
				 */
				if (i < FD_NUMPART) {
					/*
					 * set offset of 1st primary partition
					 * to 2nd cylinder
					 */
					p_new->partition_offset_sec =
					    dt->dinfo.disk_cyl_size;
					p_new->partition_offset =
					    p_new->partition_offset_sec /
					    BLOCKS_TO_MB;
					/*
					 * reduce size by 1 cylinder
					 */
					p_new->partition_size -=
					    dt->dinfo.disk_cyl_size /
					    BLOCKS_TO_MB;
					om_set_part_sec_size_from_mb(p_new);
					om_debug_print(OM_DBGLVL_INFO,
					    "%d (%02X) is the first primary "
					    "partition - will start at the 2nd "
					    "cylinder (sector %lld)\n",
					    i, p_new->partition_type,
					    p_new->partition_offset_sec);
				} else {
					/*
					 * set offset for first logical
					 * partition to start of extended
					 * partition
					 */
					p_new->partition_offset_sec =
					    extpinfo->partition_offset_sec;
					p_new->partition_offset =
					    p_new->partition_offset_sec /
					    BLOCKS_TO_MB;
					om_debug_print(OM_DBGLVL_INFO,
					    "[%d] (%02X) is the first logical "
					    "partition\n",
					    i, p_new->partition_type);
				}
			} else {
				partition_info_t	*p_prev;
				int			previous;

				previous = get_previous_used_partition(
				    new_dp->pinfo, i);

				/*
				 * previous should be always found, since check
				 * for "first used" was done in if() statement
				 * above
				 */

				assert(previous != -1);
				p_prev = &new_dp->pinfo[previous];

				p_new->partition_offset_sec =
				    p_prev->partition_offset_sec +
				    p_prev->partition_size_sec;
				p_new->partition_offset =
				    p_new->partition_offset_sec/BLOCKS_TO_MB;
			}
			/*
			 * user changed partition size in GUI or size
			 * was adjusted above.
			 * Calculate new sector size information from megabytes
			 */

			om_set_part_sec_size_from_mb(p_new);

			/*
			 * logical partitions only, must be preceded by
			 * 63 unused sectors (see man page for fdisk).
			 * push starting offset forward if 63 sectors not unused
			 * before the starting offset
			 */

			logical_start_adjust(new_dp->pinfo, i, extpinfo);
		}

		if (partition_allocation_scheme == AI_allocation) {
			/*
			 * adjust for boot partition being in 1st cylinder
			 * special allocation for GUI not needed
			 */
			if (p_new->partition_offset_sec == 0 ||
			    p_new->partition_offset_sec ==
			    LOGICAL_PARTITION_PAD) {
				/* move from 1st to 2nd cylinder */
				resize_partition(p_new,
				    dt->dinfo.disk_cyl_size);
				om_debug_print(OM_DBGLVL_INFO,
				    "%d (%02X) is the first %spartition - will "
				    "start at the 2nd cylinder (sector %lld)\n",
				    i, p_new->partition_type,
				    IS_LOG_PAR(p_new->partition_id) ?
				    "logical ":"",
				    p_new->partition_offset_sec);
			} else if (extpinfo != NULL &&
			    IS_LOG_PAR(p_new->partition_id) &&
			    extpinfo->partition_offset_sec ==
			    p_new->partition_offset_sec) {
				/* pad for logical partition */
				resize_partition(p_new, LOGICAL_PARTITION_PAD);
				om_debug_print(OM_DBGLVL_INFO,
				    "%d (%02X) is the first partition of the "
				    "extended partition - will "
				    "start 63 sectors into extended partition "
				    "(sector %lld)\n",
				    i, p_new->partition_type,
				    p_new->partition_offset_sec);
			}
		}

		/*
		 * For the GUI:
		 *
		 * If the partition overlaps with subsequent one
		 * which is in use and that partition was not changed,
		 * adjust size accordingly.
		 *
		 * If subsequent used partition was resized as well, its
		 * offset and size will be adjusted in next step, so
		 * don't modify size of current partition.
		 *
		 * If this is the last used partition, adjust its size
		 * so that it fits into available disk space
		 */

		if (partition_allocation_scheme == GUI_allocation) {
			if (!is_last_used_partition(new_dp->pinfo, i)) {
				partition_info_t *p_next_orig, *p_next_new;
				int next;

				next = get_next_used_partition(new_dp->pinfo,
				    i);

				/*
				 * next should be always found, since check for
				 * "last used" was done in if() statement above
				 */

				assert(next != -1);
				p_next_orig = &dp->pinfo[next];
				p_next_new = &new_dp->pinfo[next];

				/*
				 * next partition was resized, adjust rather
				 * that one, leave current one as is
				 */

				if (is_resized_partition(p_next_orig,
				    p_next_new))
					continue;

				if ((p_new->partition_offset_sec +
				    p_new->partition_size_sec) >
				    p_next_new->partition_offset_sec) {
					p_new->partition_size_sec =
					    p_next_new->partition_offset_sec -
					    p_new->partition_offset_sec;
					/*
					 * ensure 63 block pad before next
					 * logical partition
					 */
					if (IS_LOG_PAR(p_new->partition_id)) {
						assert(p_new->partition_size_sec
						    > LOGICAL_PARTITION_PAD);
						p_new->partition_size_sec -=
						    LOGICAL_PARTITION_PAD;
					}
					/*
					 * partition sector size was adjusted.
					 * Recalculate size in MiB as well
					 */

					om_set_part_mb_size_from_sec(p_new);

					om_debug_print(OM_DBGLVL_INFO,
					    "Partition %d (ID=%02X) overlaps "
					    "subsequent partition, "
					    "size will be adjusted to %d MB\n",
					    i, p_new->partition_type,
					    p_new->partition_size);
				}
			}
			/*
			 * if this is the last primary or logical partition,
			 * adjust the end so that it fits onto the disk or
			 * extended partition respectively
			 */
			partition_end_adjust(p_new, i, extpinfo, dt);
		}
	}

	om_debug_print(OM_DBGLVL_INFO,
	    "Adjusted partition LBA information\n");

	for (i = 0; i < nparts; i++) {
		om_debug_print(OM_DBGLVL_INFO,
		    "[%d] pid=%d ord=%d, type=%02X, beg=%llu(%lu MiB), "
		    "size=%llu(%lu MiB)\n", i,
		    new_dp->pinfo[i].partition_id,
		    new_dp->pinfo[i].partition_order,
		    new_dp->pinfo[i].partition_type,
		    new_dp->pinfo[i].partition_offset_sec,
		    new_dp->pinfo[i].partition_offset,
		    new_dp->pinfo[i].partition_size_sec,
		    new_dp->pinfo[i].partition_size);
	}

	/* release partition info if allocated if this function */

	if (dt->dparts == NULL)
		local_free_part_info(dp);

	return (new_dp);
}


/*
 * om_duplicate_disk_partition_info
 * This function will allocate space and copy the disk_parts_t structure
 * passed as a parameter.
 * Input:	om_handle_t handle - The handle returned by
 *              om_initiate_target_discovery()
 *		disk_parts_t * - Pointer to disk_parts_t. Usually the return
 *		value from get_disk_partition_info().
 * Return:	disk_parts_t * - Pointer to disk_parts_t. Space will be
 *		allocated and the data is copied and returned.
 *		NULL, if space cannot be allocated.
 */
/*ARGSUSED*/
disk_parts_t *
om_duplicate_disk_partition_info(om_handle_t handle, disk_parts_t *dparts)
{
	disk_parts_t	*dp;

	if (dparts == NULL) {
		om_set_error(OM_BAD_INPUT);
		return (NULL);
	}

	/*
	 * Allocate space for partitions and copy data
	 */
	dp = (disk_parts_t *)calloc(1, sizeof (disk_parts_t));

	if (dp == NULL) {
		om_set_error(OM_NO_SPACE);
		return (NULL);
	}

	(void) memcpy(dp, dparts, sizeof (disk_parts_t));
	dp->disk_name = strdup(dparts->disk_name);

	return (dp);
}

/*
 * om_set_disk_partition_info
 * This function will save the disk partition information passed by the
 * caller and use it for creating disk partitions during install.
 * This function should be used in conjunction with om_perform_install
 * If om_perform_install is not called, no changes in the disk will be made.
 *
 * Input:	om_handle_t handle - The handle returned by
 *		om_initiate_target_discovery()
 * 		disk_parts_t *dp - The modified disk partitions
 * Output:	None
 * Return:	OM_SUCCESS - If the disk partition information is saved
 *		OM_FAILURE - If the data cannot be saved.
 * Note:	If the partition information can't be saved, the om_errno
 *		will be set to the actual error condition. The error
 *		information can be obtained by calling om_get_errno().
 */
/*ARGSUSED*/
int
om_set_disk_partition_info(om_handle_t handle, disk_parts_t *dp)
{
	disk_target_t	*dt;

	/*
	 * Validate the input
	 */
	if (dp == NULL || dp->disk_name == NULL) {
		om_set_error(OM_BAD_INPUT);
		return (OM_FAILURE);
	}

	/*
	 * Find the disk from the cache using the diskname
	 */
	dt = find_disk_by_name(dp->disk_name);

	if (dt == NULL) {
		return (OM_FAILURE);
	}

	if (dt->dparts == NULL) {
		/*
		 * Log the information that the disk partitions are not defined
		 * before the install started and GUI has defined the partitions
		 * and saving it with orchestrator to be used during install
		 */
		om_log_print("No disk partitions defined prior to install\n");
	}

	if (allocate_target_disk_info(&dt->dinfo) != OM_SUCCESS)
		return (OM_FAILURE);

	if (committed_disk_target->dinfo.disk_name == NULL ||
	    committed_disk_target->dinfo.vendor == NULL ||
	    committed_disk_target->dinfo.serial_number == NULL) {
		goto sdpi_return;
	}
	/*
	 * Copy the partition data from the input
	 */
	committed_disk_target->dparts =
	    om_duplicate_disk_partition_info(handle, dp);
	if (committed_disk_target->dparts == NULL) {
		goto sdpi_return;
	}
	/*
	 * finishing set partition info
	 */
	log_partition_map();
	return (OM_SUCCESS);
sdpi_return:
	free_target_disk_info();
	return (OM_FAILURE);
}

/*
 * Partition editing suite
 *
 * These functions start with a description of existing partitions
 * To find partitions for a disk:
 *	-perform Target Discovery, finding disks and partitions for the disk
 *	-get partition table for disk with om_get_disk_partition_info()
 *	-if partitions exist (not NULL), set target disk information with
 *		om_set_disk_partition_info()
 *	-if no partitions exist (NULL) , create empty partition table with
 *		om_init_disk_partition_info()
 * The partition descriptions can then be edited with:
 *	om_create_partition(), om_delete_partition()
 * When new partition configuration is complete, finalize it for TI with:
 *	om_finalize_fdisk_info_for_TI()
 * Set attribute list for TI with:
 *	om_set_fdisk_target_attrs()
 *
 * om_create_partition() - create a new partition
 * partition_size_sec - size of partition in sectors
 * partition_offset_sec - offset of beginning sector
 * use_entire_disk - if true, ignore size, offset and commit entire disk
 * only Solaris partitions are created
 * returns B_TRUE if success, B_FALSE otherwise
 */
boolean_t
om_create_partition(uint8_t partition_type, uint64_t partition_offset_sec,
    uint64_t partition_size_sec, boolean_t use_entire_disk,
    boolean_t is_log_part)
{
	partition_info_t *pinfo;
	int ipart;
	char *ptype_text = "";

	assert(committed_disk_target != NULL);
	assert(committed_disk_target->dparts != NULL);

	pinfo = committed_disk_target->dparts->pinfo;
	for (ipart = 0; ipart < OM_NUMPART; ipart++, pinfo++) {
		if (partition_offset_sec != 0 &&
		    partition_offset_sec == pinfo->partition_offset_sec &&
		    pinfo->partition_size_sec != 0) { /* part already exists */
			om_debug_print(OM_DBGLVL_ERR, "Attempting to "
			    "create partition that already exists\n");
			om_set_error(OM_ALREADY_EXISTS);
			return (B_FALSE);
		}
	}
	/* find free entry */
	if (is_log_part) { /* logical partition numbering 5-36 */
		pinfo = &committed_disk_target->dparts->pinfo[4]; /* reset */
		for (ipart = FD_NUMPART; ipart < OM_NUMPART; ipart++, pinfo++)
			if (!is_used_partition(pinfo))
				break;
		if (ipart >= OM_NUMPART) {
			om_debug_print(OM_DBGLVL_ERR, "The maximum number "
			    "of logical partitions (%d) already exist\n",
			    MAX_EXT_PARTS);
			om_debug_print(OM_DBGLVL_ERR, "No more logical "
			    "partitions may be created until some are "
			    "deleted.\n");
			om_set_error(OM_BAD_INPUT);
			return (B_FALSE);
		}
	} else { /* primary partitions number 1-4 */
		pinfo = committed_disk_target->dparts->pinfo; /* reset */
		for (ipart = 0; ipart < FD_NUMPART; ipart++, pinfo++)
			if (!is_used_partition(pinfo))
				break;
		if (ipart >= FD_NUMPART) {
			om_debug_print(OM_DBGLVL_ERR, "No more primary "
			    "partitions may be created until some are "
			    "deleted.\n");
			om_set_error(OM_BAD_INPUT);
			return (B_FALSE);
		}
	}
	/* not more than one extended partition is allowed */
	if (fdisk_is_dos_extended(partition_type) &&
	    get_extended_partition_info(NULL) != NULL) {
		om_debug_print(OM_DBGLVL_ERR,
		    "Attempt to create more than one extended partition\n");
		om_set_error(OM_ALREADY_EXISTS);
		return (B_FALSE);
	}
	if (use_entire_disk) {
		partition_offset_sec = 0;
		partition_size_sec =
		    (uint64_t)committed_disk_target->dinfo.disk_size *
		    BLOCKS_TO_MB;
	}
	/* if no starting offset, select location */
	if (partition_offset_sec == (uint64_t)-1LL) {
		struct free_region *pfree_region;

		om_debug_print(OM_DBGLVL_INFO,
		    "finding unused region of size=%s\n",
		    part_size_or_max(partition_size_sec));
		pfree_region = find_unused_region_of_size(partition_size_sec,
		    is_log_part);
		if (pfree_region == NULL) {
			om_debug_print(OM_DBGLVL_ERR,
			    "failure to find unused region of size %s\n",
			    part_size_or_max(partition_size_sec));
			om_set_error(OM_ALREADY_EXISTS);
			return (B_FALSE);
		}
		pinfo->partition_offset_sec = pfree_region->free_offset;
		if (partition_size_sec == OM_MAX_SIZE)
			partition_size_sec = pfree_region->free_size;
	} else {
		/* if size set to OM_MAX_SIZE in manifest, take entire disk */
		if (partition_size_sec == OM_MAX_SIZE)
			if (is_log_part) {
				partition_info_t *extpinfo;

				om_debug_print(OM_DBGLVL_INFO,
				    "allocating entire extended partition "
				    "for logical partition\n");
				extpinfo = get_extended_partition_info(NULL);
				if (extpinfo == NULL) {
					om_debug_print(OM_DBGLVL_ERR,
					    "system error: failed to find "
					    "extended partition definition\n");
					om_set_error(OM_NO_PARTITION_FOUND);
					return (B_FALSE);
				}
				partition_size_sec =
				    (uint64_t)extpinfo->partition_size *
				    BLOCKS_TO_MB;
			} else
				partition_size_sec =
				    (uint64_t)
				    committed_disk_target->dinfo.disk_size *
				    BLOCKS_TO_MB;
		pinfo->partition_offset_sec = partition_offset_sec;
	}
	/* check type of partition to create - fdisk supported? */
	switch (partition_type) {
	case SUNIXOS2:		/* Solaris */
		ptype_text = "Solaris2 ";
		pinfo->content_type = OM_CTYPE_SOLARIS;
		break;
	case SUNIXOS:		/* Solaris UNIX partition */
		ptype_text = "Solaris ";
		pinfo->content_type = OM_CTYPE_SOLARIS;
		break;
	case FDISK_LINUX:	/* Linux */
	case FDISK_LINUXDNAT:	/* Linux native (sharing disk with DRDOS) */
	case FDISK_LINUXNAT:	/* Linux native */
		pinfo->content_type = OM_CTYPE_UNKNOWN; /* content unknown */
		ptype_text = "Linux ";
		break;
	case FDISK_LINUXDSWAP:	/* Linux swap (sharing disk w/ DRDOS) */
		pinfo->content_type = OM_CTYPE_UNKNOWN; /* contnet unknown */
		ptype_text = "Linux swap ";
		break;
	/* any other types that fdisk supports */
	case DOSOS12:		/* DOS partition, 12-bit FAT */
	case DOSOS16:		/* DOS partition, 16-bit FAT */
	case EXTDOS:		/* EXT-DOS partition */
	case DOSHUGE:		/* Huge DOS partition  > 32MB */
	case FDISK_IFS:		/* Installable File System (IFS): HPFS & NTFS */
	case FDISK_AIXBOOT:	/* AIX Boot */
	case FDISK_AIXDATA:	/* AIX Data */
	case FDISK_OS2BOOT:	/* OS/2 Boot Manager */
	case FDISK_WINDOWS:	/* Windows 95 FAT32 (up to 2047GB) */
	case FDISK_EXT_WIN:	/* Windows 95 FAT32 (extended-INT13) */
	case FDISK_FAT95:	/* DOS 16-bit FAT, LBA-mapped */
	case FDISK_EXTLBA:	/* Extended partition, LBA-mapped */
	case DIAGPART:		/* Diagnostic boot partition (OS independent) */
	case FDISK_CPM:		/* CP/M */
	case DOSDATA:		/* DOS data partition */
	case OTHEROS:
	case UNIXOS:		/* UNIX V.x partition */
	case FDISK_NOVELL3:	/* Novell Netware 3.x and later */
	case FDISK_QNX4:	/* QNX 4.x */
	case FDISK_QNX42:	/* QNX 4.x 2nd part */
	case FDISK_QNX43:	/* QNX 4.x 3rd part */
	case FDISK_NTFSVOL1:	/* NTFS volume set 1 */
	case FDISK_NTFSVOL2:	/* NTFS volume set 2 */
	case FDISK_BSD:		/* BSD/386, 386BSD, NetBSD, FreeBSD, OpenBSD */
	case FDISK_NEXTSTEP:	/* NeXTSTEP */
	case FDISK_BSDIFS:	/* BSDI file system */
	case FDISK_BSDISWAP:	/* BSDI swap */
		pinfo->content_type = OM_CTYPE_UNKNOWN; /* content unknown */
		break;
	default:
		om_debug_print(OM_DBGLVL_ERR,
		    "Unsupported partition type %d requested.\n",
		    partition_type);
		return (B_FALSE);
	}
	if (!build_free_space_table(is_log_part)) /* checks for overlap */
		return (B_FALSE);

	pinfo->partition_type = partition_type;
	pinfo->partition_size_sec = partition_size_sec;
	pinfo->partition_offset = pinfo->partition_offset_sec / BLOCKS_TO_MB;
	pinfo->partition_size = pinfo->partition_size_sec / BLOCKS_TO_MB;
	pinfo->partition_id = ipart + 1;

	om_debug_print(OM_DBGLVL_INFO,
	    "will create partition of type %s(%d) size=%lld offset=%lld\n",
	    ptype_text, partition_type, pinfo->partition_size_sec,
	    pinfo->partition_offset_sec);
	om_debug_print(OM_DBGLVL_INFO, "adding partition in slot %d\n", ipart);
	log_partition_map();
	return (B_TRUE);
}

/*
 * om_delete_partition() - delete an existing partition
 *
 * partition_id - number of partition to delete
 *	- or if zero -
 * partition_size_sec - size of partition in sectors
 * partition_offset_sec - offset of begininng sector
 *
 * returns B_TRUE if success, B_FALSE otherwise
 */
boolean_t
om_delete_partition(uint8_t partition_id, uint64_t partition_offset_sec,
    uint64_t partition_size_sec)
{
	partition_info_t *pinfo;
	int ipart;

	assert(committed_disk_target != NULL);
	assert(committed_disk_target->dparts != NULL);

	/*
	 * delete by ID (1-36) if provided
	 */
	if (partition_id != 0) {
		om_debug_print(OM_DBGLVL_INFO,
		    "to delete partition %d\n", (int)partition_id);
		ipart = partition_index_by_id(partition_id);
		if (ipart == -1) {
			om_debug_print(OM_DBGLVL_ERR,
			    "Could not find partition %d to delete. "
			    "Assumed deleted\n", partition_id);
			return (B_TRUE);
		}
		/*
		 * if partition to delete is extended partition,
		 *	also delete any logical partitions from target table
		 */
		pinfo = committed_disk_target->dparts->pinfo;
		if (fdisk_is_dos_extended(pinfo[ipart].partition_type))
			delete_all_logical_partitions();
		/*
		 * delete indicated partition
		 */
		mark_for_deletion_by_index(ipart);
		om_log_print("partition ID=%d marked for deletion\n",
		    (int)partition_id);
		return (B_TRUE);
	}
	/*
	 * otherwise, delete by sector location
	 */
	om_debug_print(OM_DBGLVL_INFO,
	    "to delete partition by location: offset=%lld size=%lld\n",
	    partition_offset_sec, partition_size_sec);
	pinfo = committed_disk_target->dparts->pinfo;
	for (ipart = 0; ipart < OM_NUMPART; ipart++) {
		if (partition_offset_sec == pinfo[ipart].partition_offset_sec &&
		    (partition_size_sec == 0LL || /* if length specified */
		    partition_size_sec == pinfo[ipart].partition_size_sec)) {
			/*
			 * if partition to delete is extended partition,
			 *	also delete any logical partitions from target
			 */
			if (fdisk_is_dos_extended(pinfo[ipart].partition_type))
				delete_all_logical_partitions();
			mark_for_deletion_by_index(ipart);
			return (B_TRUE);
		}
	}
	om_debug_print(OM_DBGLVL_ERR,
	    "Failed to locate specified partition to delete at starting sector "
	    "%lld with size in sectors=%lld\n",
	    partition_offset_sec, partition_size_sec);
	om_set_error(OM_BAD_INPUT);
	return (B_FALSE);
}

/*
 * given partition index (0-35), mark the partition for deletion
 *	by removing it from the disk target table
 * It is assumed that the index is valid and points to a used partition
 * NOTE: this function deletes an array element by index, not by partition
 *	number, which is the index + 1
 */
static void
mark_for_deletion_by_index(int ipart)
{
	partition_info_t *pinfo = committed_disk_target->dparts->pinfo;

	om_debug_print(OM_DBGLVL_INFO, "marking partition with index "
	    "%d (0-%d) for deletion-type=%d\n", ipart, OM_NUMPART - 1,
	    pinfo[ipart].partition_type);
	if (pinfo[ipart].partition_type == SUNIXOS2)
		om_invalidate_slice_info();
	/*
	 * clear entry
	 */
	set_partition_unused(&pinfo[ipart]);
}

static void
delete_all_logical_partitions()
{
	int ipart;
	partition_info_t *pinfo = committed_disk_target->dparts->pinfo;

	for (ipart = 0; ipart < OM_NUMPART; ipart++, pinfo++)
		if (IS_LOG_PAR(pinfo->partition_id))
			mark_for_deletion_by_index(ipart);
}
/*
 * om_finalize_fdisk_info_for_TI() - write out partition table containing edits
 * performs adjustments to layout:
 *	-eliminate use of first cylinder for x86
 *	-eliminate overlapping
 *	-allow gaps between partitions
 * returns B_TRUE if success, B_FALSE otherwise
 * side effect: may modify target disk partition info
 */
boolean_t
om_finalize_fdisk_info_for_TI()
{
	disk_parts_t *dparts = committed_disk_target->dparts;
	disk_parts_t *newdparts;

	assert(committed_disk_target->dinfo.disk_name != NULL);

	newdparts = om_validate_and_resize_disk_partitions(0, dparts,
	    AI_allocation);
	if (newdparts == NULL) {
		om_debug_print(OM_DBGLVL_ERR,
		    "Partition information is invalid\n");
		return (B_FALSE);
	}
	committed_disk_target->dparts = newdparts;
	om_debug_print(OM_DBGLVL_INFO,
	    "om_finalize_fdisk_info_for_TI:%s partition 0 %ld MB disk "
	    "%ld MB %lld sectors\n", whole_disk ? "entire disk":"",
	    dparts->pinfo[0].partition_size,
	    committed_disk_target->dinfo.disk_size,
	    committed_disk_target->dinfo.disk_size_sec);
	log_partition_map();
	return (B_TRUE);
}

/*
 * set disk partition info initially for no partitions
 * allocate on heap
 * given disk name, set all partitions empty
 * return pointer to disk partition info
 * return NULL if memory allocation failure
 */
disk_parts_t *
om_init_disk_partition_info(disk_info_t *di)
{
	char *disk_name;
	disk_parts_t *dp;
	int i;

	disk_name = di->disk_name;
	assert(disk_name != NULL);

	dp = calloc(1, sizeof (disk_parts_t));
	if (dp == NULL) {
		om_set_error(OM_NO_SPACE);
		return (NULL);
	}
	dp->disk_name = strdup(disk_name);
	if (dp->disk_name == NULL) {
		free(dp);
		om_set_error(OM_NO_SPACE);
		return (NULL);
	}
	/* mark all unused */
	for (i = 0; i < OM_NUMPART; i++)
		set_partition_unused(&dp->pinfo[i]);
	return (dp);
}

/*
 * om_create_target_partition_info_if_absent(): initialize a target disk
 *	partition struct if not yet initialized
 * This was designed for the case of no partition table on disk and no
 *	customizations were provided by the user
 * side effect: if no target disk partitions have been found or specified,
 *	initialize the target disk information to use the entire target disk
 *	for a single partition
 */
void
om_create_target_partition_info_if_absent()
{
	partition_info_t *pinfo;
	disk_info_t *di;

	assert(committed_disk_target != NULL);
	assert(committed_disk_target->dparts != NULL);

	pinfo = committed_disk_target->dparts->pinfo;
	if (pinfo != NULL && get_first_used_partition(pinfo) != -1)
		return;	/* target partition table has already initialized */
	om_debug_print(OM_DBGLVL_INFO,
	    "No partition info - Creating target disk partition table - "
	    "use entire target disk\n");
	/* mark first partition to be Solaris2 */
	di = &committed_disk_target->dinfo;
	pinfo->partition_id = 1;
	pinfo->content_type = OM_CTYPE_SOLARIS;
	pinfo->partition_type = SUNIXOS2;
	pinfo->partition_size = di->disk_size;
	pinfo->partition_size_sec = (uint64_t)di->disk_size * BLOCKS_TO_MB;
}

/*
 * om_set_fdisk_target_attrs
 * This function sets the appropriate fdisk attributes for target instantiation.
 * Input:	nvlist_t *target_attrs - list to add attributes to
 * Output:	None
 * Return:	errno - see orchestrator_api.h
 */
int
om_set_fdisk_target_attrs(nvlist_t *list, char *diskname)
{
	disk_target_t	*dt;
	disk_parts_t	*cdp;
	int		i;
	boolean_t	preserve_all;

	uint8_t		part_ids[OM_NUMPART], part_active_flags[OM_NUMPART];
	uint64_t	part_offsets[OM_NUMPART], part_sizes[OM_NUMPART];
	boolean_t	preserve_array[OM_NUMPART];
	partition_info_t	*install_partition = NULL;
	int		nparts = (get_extended_partition_info(NULL) == NULL ?
	    FD_NUMPART:OM_NUMPART);

	om_set_error(OM_SUCCESS);

	/*
	 * We have all the data from the GUI committed at this point.
	 * Gather it, and set the attributes.
	 */

	for (dt = system_disks; dt != NULL; dt = dt->next) {
		if (streq(dt->dinfo.disk_name, diskname)) {
			break;
		}
	}

	if (dt == NULL) {
		om_log_print("Bad target disk name\n");
		om_set_error(OM_BAD_DISK_NAME);
		return (-1);
	}

	if (committed_disk_target == NULL) {
		om_log_print("Disk is not changed\n");
		preserve_all = B_TRUE;

		/*
		 * No existing partitions and No new partitions.
		 * we can't proceed with install
		 */
		if (dt->dparts == NULL) {
			om_log_print("Disk is empty - doesn't contain "
			    "partitions\n");

			om_set_error(OM_NO_PARTITION_FOUND);
			return (-1);
		}

		cdp = dt->dparts;
	} else {
		om_log_print("Disk was changed\n");
		preserve_all = B_FALSE;

		if (committed_disk_target->dparts == NULL) {
			om_log_print("Configuration of new partitions not "
			    "available\n");

			om_set_error(OM_NO_PARTITION_FOUND);
			return (-1);
		}

		cdp = committed_disk_target->dparts;
	}

	/*
	 * Make sure that there is a Solaris or Solaris 2 partition.
	 */

	for (i = 0; i < nparts; i++) {
		if (cdp->pinfo[i].partition_type == SUNIXOS2 ||
		    (cdp->pinfo[i].partition_type == SUNIXOS &&
		    cdp->pinfo[i].content_type != OM_CTYPE_LINUXSWAP)) {
			om_log_print("Disk contains valid Solaris partition\n");

			/*
			 * Check whether the partition type is legacy Solaris
			 * (SUNIXOS) or new Solaris2 (SUNIXOS2). If it is
			 * SUNIXOS, convert it to SUNIXOS2.
			 */
			if (cdp->pinfo[i].partition_type == SUNIXOS) {
				om_log_print(
				    "Disk contains legacy Solaris partition. "
				    "It will be converted to Solaris2\n");

				/*
				 * If user didn't make any changes to the
				 * original partition configuration
				 * (committed_disk_target == NULL), it
				 * is necessary to create a copy of the
				 * original partition configuration and
				 * change the partition type.
				 */

				if (committed_disk_target == NULL) {
					om_debug_print(OM_DBGLVL_INFO,
					    "committed_disk_target == NULL, "
					    "copy of original partition "
					    "configuration will be created\n");

					/*
					 * First parameter (handle) is
					 * currently unused, set to '0'
					 */
					if (om_set_disk_partition_info(0,
					    cdp) != OM_SUCCESS) {
						om_debug_print(OM_DBGLVL_ERR,
						    "Couldn't duplicate "
						    "partition "
						    "configuration\n");

						return (-1);
					}

					cdp = committed_disk_target->dparts;
				}

				cdp->pinfo[i].partition_type = SUNIXOS2;
				preserve_all = B_FALSE;
			}
			install_partition = &cdp->pinfo[i];
			break;
		}
	}

	/*
	 * No Solaris partition. Do not proceed
	 */

	if (i == nparts) {
		om_log_print("Disk doesn't contain valid Solaris partition\n");

		om_set_error(OM_NO_PARTITION_FOUND);
		return (-1);
	}

	/*
	 * Solaris partition found - take a look at the partition size
	 * and decide, if there is enough space to create swap and
	 * dump devices
	 */

	om_debug_print(OM_DBGLVL_INFO,
	    "Recommended disk size is %llu MiB\n",
	    om_get_recommended_size(NULL, NULL));

	om_debug_print(OM_DBGLVL_INFO,
	    "Install partition size = %lu MiB\n",
	    install_partition != NULL ?
	    install_partition->partition_size : 0);

	if (install_partition == NULL) {
		om_debug_print(OM_DBGLVL_WARN,
		    "Couldn't obtain size of install partition, swap&dump "
		    "won't be created\n");

		create_swap_and_dump = B_FALSE;
	} else if (install_partition->partition_size <
	    om_get_recommended_size(NULL, NULL) - OVERHEAD_MB) {

		om_debug_print(OM_DBGLVL_INFO,
		    "Install partition is too small, swap&dump won't "
		    "be created\n");

		create_swap_and_dump = B_FALSE;
	} else {
		om_debug_print(OM_DBGLVL_INFO,
		    "Size of install partition is sufficient for creating "
		    "swap&dump\n");

		create_swap_and_dump = B_TRUE;
	}

	/*
	 * set target type
	 */

	if (nvlist_add_uint32(list, TI_ATTR_TARGET_TYPE,
	    TI_TARGET_TYPE_FDISK) != 0) {
		(void) om_log_print("Couldn't add TI_ATTR_TARGET_TYPE to"
		    "nvlist\n");

		om_set_error(OM_NO_SPACE);
		return (-1);
	}

	if (nvlist_add_string(list, TI_ATTR_FDISK_DISK_NAME,
	    diskname) != 0) {
		om_log_print("Couldn't add FDISK_DISK_NAME attr\n");

		om_set_error(OM_NO_SPACE);
		return (-1);
	}

	if (nvlist_add_boolean_value(list, TI_ATTR_FDISK_WDISK_FL,
	    whole_disk) != 0) {
		om_log_print("Couldn't add WDISK_FL attr\n");

		om_set_error(OM_NO_SPACE);
		return (-1);
	}

	om_log_print("whole_disk = %d\n", whole_disk);
	om_log_print("diskname set = %s\n", diskname);

	/*
	 * If "whole disk", no more attributes need to be set
	 */

	if (whole_disk)
		return (0);

	/* add number of partitions to be created */

	if (nvlist_add_uint16(list, TI_ATTR_FDISK_PART_NUM,
	    nparts) != 0) {
		om_log_print("Couldn't add FDISK_PART_NAME attr\n");

		om_set_error(OM_NO_SPACE);
		return (-1);
	}

	/*
	 * if no changes should be done to fdisk partition table
	 * set "preserve" flag for all partitions
	 */

	if (preserve_all) {
		om_log_print("No changes will be done to the partition "
		    "table\n");

		for (i = 0; i < nparts; i++)
			preserve_array[i] = B_TRUE;

		/* preserve flags */

		if (nvlist_add_boolean_array(list, TI_ATTR_FDISK_PART_PRESERVE,
		    preserve_array, nparts) != 0) {
			om_log_print("Couldn't add FDISK_PART_PRESERVE attr\n");
			om_set_error(OM_NO_SPACE);
			return (-1);
		}

		return (0);
	}

	/*
	 * check whether disk partitions are changed for this install
	 * The caller should have called to commit the changes
	 */

	if (committed_disk_target->dparts == NULL) {
		return (-1);
	}

	/*
	 * The disk we got for install is different
	 * from the disk information committed before.
	 * So return error.
	 */

	if (!streq(diskname, committed_disk_target->dinfo.disk_name)) {
		return (-1);
	}

	/*
	 * Now find out the changed partitions
	 */

	cdp = committed_disk_target->dparts;

	om_debug_print(OM_DBGLVL_INFO, "Committed partition LBA information\n");
	for (i = 0; i < nparts; i++) {
		om_debug_print(OM_DBGLVL_INFO,
		    "[%d] pos=%d, id=%02X, beg=%llu(%lu MiB), "
		    "size=%llu(%lu MiB)\n", i,
		    cdp->pinfo[i].partition_id,
		    cdp->pinfo[i].partition_type,
		    cdp->pinfo[i].partition_offset_sec,
		    cdp->pinfo[i].partition_offset,
		    cdp->pinfo[i].partition_size_sec,
		    cdp->pinfo[i].partition_size);
	}

	/*
	 * Partitions are sorted by offset for now.
	 *
	 * For TI purposes sort partitions according to position
	 * in fdisk partition table.
	 */

	/*
	 * Mark all entries as unused and then fill in used positions
	 *
	 * Set ID to UNUSED for unused entries. Otherwise fdisk(1M)
	 * refuses to create partition table.
	 *
	 * Initially assume that nothing will be preserved.
	 */

	for (i = 0; i < nparts; i++) {
		part_ids[i] = UNUSED;
		part_active_flags[i] =
		    part_offsets[i] = part_sizes[i] = 0;

		preserve_array[i] = B_FALSE;
	}

	for (i = 0; i < nparts; i++) {
		uint64_t	size_new = cdp->pinfo[i].partition_size;
		uint8_t		type_new = cdp->pinfo[i].partition_type;
		int		pos = i;

		/* Skip unused entries */

		if (pos == -1 || size_new == 0)
			continue;

		/*
		 * If size and type didn't change, preserve the partition.
		 * "move" operation (only offset changed) is not supported.
		 */

		/*
		 * If disk had empty partition table, don't compare,
		 * just create the partition
		 */

		if ((dt->dparts != NULL) &&
		    (dt->dparts->pinfo[i].partition_size == size_new) &&
		    (dt->dparts->pinfo[i].partition_type == type_new)) {
			preserve_array[pos] = B_TRUE;
		}

		part_ids[pos] = type_new;
		part_active_flags[pos] = 0;
		part_offsets[pos] = cdp->pinfo[i].partition_offset_sec;
		part_sizes[pos] = cdp->pinfo[i].partition_size_sec;
	}

	/*
	 * Add partition geometry to the list of attributes
	 */

	/* ID */

	if (nvlist_add_uint8_array(list, TI_ATTR_FDISK_PART_IDS,
	    part_ids, nparts) != 0) {
		om_log_print("Couldn't add FDISK_PART_IDS attr\n");
		om_set_error(OM_NO_SPACE);
		return (-1);
	}

	/* ACTIVE */

	if (nvlist_add_uint8_array(list, TI_ATTR_FDISK_PART_ACTIVE,
	    part_active_flags, nparts) != 0) {
		om_log_print("Couldn't add FDISK_PART_ACTIVE attr\n");
		om_set_error(OM_NO_SPACE);
		return (-1);
	}

	/* offset */

	if (nvlist_add_uint64_array(list, TI_ATTR_FDISK_PART_RSECTS,
	    part_offsets, nparts) != 0) {
		om_log_print("Couldn't add FDISK_PART_RSECTS attr\n");
		om_set_error(OM_NO_SPACE);
		return (-1);
	}

	/* size */

	if (nvlist_add_uint64_array(list, TI_ATTR_FDISK_PART_NUMSECTS,
	    part_sizes, nparts) != 0) {
		om_log_print("Couldn't add FDISK_PART_NUMSECTS attr\n");
		om_set_error(OM_NO_SPACE);
		return (-1);
	}

	/* preserve flags */

	if (nvlist_add_boolean_array(list, TI_ATTR_FDISK_PART_PRESERVE,
	    preserve_array, nparts) != 0) {
		om_log_print("Couldn't add FDISK_PART_PRESERVE attr\n");
		om_set_error(OM_NO_SPACE);
		return (-1);
	}

	om_set_error(OM_SUCCESS);
	return (0);
}

/*
 * find best fit among blocks of unused space that has at least partition_size
 *	sectors unallocated
 * if partition_size is 0, return largest free region
 */
static struct free_region *
find_unused_region_of_size(uint64_t partition_size, boolean_t is_log_part)
{
	assert(committed_disk_target != NULL);

	if (!build_free_space_table(is_log_part))
		return (NULL);
	/*
	 * if partition size unspecified (signaled when zero)
	 * find largest free space
	 * otherwise find best fit for specified size
	 */
	return (partition_size == OM_MAX_SIZE ?
	    find_largest_free_region(is_log_part) :
	    find_free_region_best_fit(partition_size, is_log_part));
}

/*
 * do a sorted insertion of used space in partition
 */
static void
insertion_sort_partition_info(partition_info_t *psinfo)
{
	int isl;

	for (isl = 0; isl < n_sorted_parts; isl++)
		if (sorted_parts[isl].partition_offset_sec >
		    psinfo->partition_offset_sec)
			break;
	/* safe push downward */
	(void) memmove(&sorted_parts[isl + 1], &sorted_parts[isl],
	    (n_sorted_parts - isl) * sizeof (sorted_parts[0]));
	/* move in new partition info entry */
	sorted_parts[isl].partition_id = psinfo->partition_id;
	sorted_parts[isl].partition_offset_sec = psinfo->partition_offset_sec;
	sorted_parts[isl].partition_size_sec = psinfo->partition_size_sec;
	n_sorted_parts++;
}

/*
 * make table of used partition space taken from disk target
 */
static void
sort_used_regions(boolean_t is_log_part)
{
	partition_info_t *psinfo;
	int isl;

	n_sorted_parts = 0;
	for (psinfo = &committed_disk_target->dparts->pinfo[0],
	    isl = 0; isl < OM_NUMPART; isl++, psinfo++) {
		if (psinfo->partition_size == 0)
			continue;
		/*
		 * if logical partition is to be created,
		 *	do not include non-logical partitions in table
		 */
		if (is_log_part && !IS_LOG_PAR(psinfo->partition_id))
			continue;
		/*
		 * if primary partition is to be created,
		 *	do not include logical partitions in table
		 */
		if (!is_log_part && IS_LOG_PAR(psinfo->partition_id))
			continue;
		insertion_sort_partition_info(psinfo);
	}
}

/*
 * populate a table with entries for each region of free space in the
 *	target disk partition table
 * returns B_FALSE if any problems
 *	-overlapping in the partitions was detected
 *	-disk size unknown
 * returns B_TRUE if no problems were detected
 * side effects:
 *	sets n_fragments: number of free space fragments
 *	calls append_free_space_table() to add free regions
 */
static boolean_t
build_free_space_table(boolean_t is_log_part)
{
	int isl;
	uint64_t free_size;
	uint64_t max_size_sec;
	uint64_t starting_sec = 0;
	partition_info_t *extpinfo;

	bzero(&sorted_parts, sizeof (sorted_parts));
	if (is_log_part) {
		extpinfo = get_extended_partition_info(NULL);
		if (extpinfo == NULL) {
			om_debug_print(OM_DBGLVL_ERR,
			    "system error: failed to find "
			    "extended partition definition\n");
			return (B_FALSE);
		}
		starting_sec = extpinfo->partition_offset_sec;
		max_size_sec = extpinfo->partition_size_sec;
		om_debug_print(OM_DBGLVL_INFO,
		    "extended partition: start %lld size %lld\n",
		    starting_sec, max_size_sec);
		assert(max_size_sec != 0);
	} else {
		uint64_t disk_size_sec;

		disk_size_sec = committed_disk_target->dinfo.disk_size_sec;
		if (disk_size_sec == 0) /* sometimes sectors field is blank */
			disk_size_sec =		/* take from MB field */
			    (uint64_t)committed_disk_target->dinfo.disk_size *
			    BLOCKS_TO_MB;
		if (disk_size_sec == 0) {
			om_debug_print(OM_DBGLVL_ERR, "User is requesting "
			    "partition changes, requiring a known disk size, "
			    "but the target disk size (%s) is unknown. "
			    "Cannot continue installation.\n",
			    committed_disk_target->dinfo.disk_name);
			return (B_FALSE);
		}
		max_size_sec = disk_size_sec;
	}

	/* sort partition table by starting offset */
	sort_used_regions(is_log_part);
	n_fragments = 0; /* reset number of free regions */

	/* if no partitions used, set entire partition as being free */
	if (n_sorted_parts == 0) {
		append_free_space_table(starting_sec, max_size_sec);
		return (B_TRUE);
	}
	/* check for space before first partition */
	if (is_log_part) {
		if (sorted_parts[0].partition_offset_sec > starting_sec +
		    LOGICAL_PARTITION_PAD)
			append_free_space_table(starting_sec,
			    sorted_parts[0].partition_offset_sec);
	} else if (sorted_parts[0].partition_offset_sec > starting_sec) {
		append_free_space_table(starting_sec,
		    sorted_parts[0].partition_offset_sec);
	}
	for (isl = 0; isl < n_sorted_parts - 1; isl++) {
		/* does end of current partition overlap start of next one? */
		if (PARTITION_END(isl) >
		    sorted_parts[isl + 1].partition_offset_sec) {
			om_debug_print(OM_DBGLVL_ERR, "User is requesting "
			    "overlapping partitions, which is illegal.\n");
			return (B_FALSE);
		}
		/* compute space between partitions */
		free_size = sorted_parts[isl + 1].partition_offset_sec -
		    PARTITION_END(isl);
		if (free_size > 0)
			append_free_space_table(PARTITION_END(isl), free_size);
	}
	/* check for any free space between last partition and end of disk */
	free_size = max_size_sec - PARTITION_END(n_sorted_parts - 1);
	if (free_size > 0)
		append_free_space_table(
		    PARTITION_END(n_sorted_parts - 1), free_size);
	return (B_TRUE);
}

/*
 * append offset and length of a block of free space
 */
static void
append_free_space_table(uint64_t free_offset, uint64_t free_size)
{
	/* should never exceed table size */
	if (n_fragments >=
	    sizeof (free_space_table) / sizeof (free_space_table[0]))
		return;

	free_space_table[n_fragments].free_offset = free_offset;
	free_space_table[n_fragments].free_size = free_size;
	om_debug_print(OM_DBGLVL_INFO,
	    "free space table %d offset %lld size %lld\n",
	    n_fragments, free_offset, free_size);
	n_fragments++;
}

/*
 * find largest contiguous space not in other partitions in free space table
 * must have previous call to build_free_space_table()
 * return size + offset of region or NULL if none found
 */
static struct free_region *
find_largest_free_region(boolean_t is_log_part)
{
	struct free_region *pregion;
	struct free_region *largest_region = NULL;
	int ireg;

	for (pregion = free_space_table, ireg = 0;
	    ireg < n_fragments; ireg++, pregion++) {
		if (largest_region == NULL ||
		    pregion->free_size > largest_region->free_size)
			largest_region = pregion;
	}
	if (largest_region != NULL && is_log_part) {
		largest_region->free_offset += LOGICAL_PARTITION_PAD;
		largest_region->free_size -= LOGICAL_PARTITION_PAD;
	}
	return (largest_region);
}

/*
 * find contiguous space that fits requested size most closely
 * must have previous call to build_free_space_table()
 * return size + offset of region or NULL if none found
 */
static struct free_region *
find_free_region_best_fit(uint64_t partition_size, boolean_t is_log_part)
{
	struct free_region *pregion;
	struct free_region *best_fit = NULL;
	int ireg;

	/*
	 * logical partitions require 63 sectors before each one
	 * request size 63 sectors
	 */
	if (is_log_part)
		partition_size += LOGICAL_PARTITION_PAD;
	for (pregion = free_space_table, ireg = 0;
	    ireg < n_fragments; ireg++, pregion++) {
		if (best_fit == NULL) { /* find first fit */
			if (pregion->free_size >= partition_size)
				best_fit = pregion;
			continue;
		}
		/* check if better fit */
		if (pregion->free_size > partition_size &&
		    pregion->free_size < best_fit->free_size)
			best_fit = pregion;
	}
	/*
	 * since logical partitions require padding,
	 * return the actual size of the block minus the padding
	 */
	if (best_fit != NULL && is_log_part) {
		best_fit->free_offset += LOGICAL_PARTITION_PAD;
		best_fit->free_size -= LOGICAL_PARTITION_PAD;
	}
	return (best_fit);
}

/*
 * dump from sorted non-exempt slice table
 */
static void
log_used_regions()
{
	int isl;

	om_debug_print(OM_DBGLVL_INFO, "Sorted partitions table:\n");
	if (n_sorted_parts == 0) {
		om_debug_print(OM_DBGLVL_INFO,
		    "\tno partitions in sorted table\n");
		return;
	}
	om_debug_print(OM_DBGLVL_INFO,
	    "\tpartition\toffset\tsize\toffset+size\n");
	for (isl = 0; isl < n_sorted_parts; isl++) {
		om_debug_print(OM_DBGLVL_INFO, "\t%d\t%lld\t%lld\t%lld\n",
		    sorted_parts[isl].partition_id,
		    sorted_parts[isl].partition_offset_sec,
		    sorted_parts[isl].partition_size_sec,
		    sorted_parts[isl].partition_offset_sec +
		    sorted_parts[isl].partition_size_sec);
	}
}

/*
 * dump free space entries from table
 */
static void
log_free_space_table()
{
	int i;

	om_debug_print(OM_DBGLVL_INFO,
	    "Free partition space fragments - count %d\n",
	    n_fragments);
	if (n_fragments == 0) {
		om_debug_print(OM_DBGLVL_INFO, "\tno free space\n");
		return;
	}
	om_debug_print(OM_DBGLVL_INFO, "\toffset\tsize\tnoffset+size\n");
	for (i = 0; i < n_fragments; i++)
		om_debug_print(OM_DBGLVL_INFO, "\t%lld\t%lld\t%lld\n",
		    free_space_table[i].free_offset,
		    free_space_table[i].free_size,
		    free_space_table[i].free_offset +
		    free_space_table[i].free_size);
}

/*
 * end of slice editing suite
 */
