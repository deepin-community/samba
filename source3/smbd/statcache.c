/*
   Unix SMB/CIFS implementation.
   stat cache code
   Copyright (C) Andrew Tridgell 1992-2000
   Copyright (C) Jeremy Allison 1999-2007
   Copyright (C) Andrew Bartlett <abartlet@samba.org> 2003
   Copyright (C) Volker Lendecke 2007

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

#include "includes.h"
#include "../lib/util/memcache.h"
#include "smbd/smbd.h"
#include "messages.h"
#include "serverid.h"
#include "smbprofile.h"
#include <tdb.h>

#define STAT_CACHE_TWRP_TOKEN "%016" PRIx64 "@%s"
#define STAT_CACHE_TWRP_TOKEN_LEN 17

/****************************************************************************
 Stat cache code used in unix_convert.
*****************************************************************************/

/**
 * Add an entry into the stat cache.
 *
 * @param full_orig_name       The original name as specified by the client
 * @param orig_translated_path The name on our filesystem.
 *
 * @note Only the first strlen(orig_translated_path) characters are stored
 *       into the cache.  This means that full_orig_name will be internally
 *       truncated.
 *
 */

void stat_cache_add( const char *full_orig_name,
		const char *translated_path_in,
		NTTIME twrp,
		bool case_sensitive)
{
	size_t translated_path_length;
	char *translated_path = NULL;
	char *original_path;
	size_t original_path_length;
	TALLOC_CTX *ctx = talloc_tos();

	if (!lp_stat_cache()) {
		return;
	}

	/*
	 * Don't cache trivial valid directory entries such as . and ..
	 */

	if ((*full_orig_name == '\0')
	    || ISDOT(full_orig_name) || ISDOTDOT(full_orig_name)) {
		return;
	}

	translated_path = talloc_asprintf(ctx,
					  STAT_CACHE_TWRP_TOKEN,
					  twrp,
					  translated_path_in);
	if (translated_path == NULL) {
		return;
	}

	/*
	 * If we are in case insentive mode, we don't need to
	 * store names that need no translation - else, it
	 * would be a waste.
	 */

	if (!case_sensitive && (strcmp(full_orig_name, translated_path) == 0)) {
		TALLOC_FREE(translated_path);
		return;
	}

	/*
	 * Remove any trailing '/' characters from the
	 * translated path.
	 */

	translated_path_length = strlen(translated_path);

	if(translated_path[translated_path_length-1] == '/') {
		translated_path_length--;
	}

	if(case_sensitive) {
		original_path = talloc_asprintf(ctx,
						STAT_CACHE_TWRP_TOKEN,
						twrp,
						full_orig_name);
	} else {
		char *upper = NULL;

		upper = talloc_strdup_upper(ctx, full_orig_name);
		if (upper == NULL) {
			TALLOC_FREE(translated_path);
			return;
		}
		original_path = talloc_asprintf(ctx,
						STAT_CACHE_TWRP_TOKEN,
						twrp,
						upper);
		TALLOC_FREE(upper);
	}

	if (!original_path) {
		TALLOC_FREE(translated_path);
		return;
	}

	original_path_length = strlen(original_path);

	if(original_path[original_path_length-1] == '/') {
		original_path[original_path_length-1] = '\0';
		original_path_length--;
	}

	if (original_path_length != translated_path_length) {
		if (original_path_length < translated_path_length) {
			DEBUG(0, ("OOPS - tried to store stat cache entry "
			"for weird length paths [%s] %lu and [%s] %lu)!\n",
				  original_path,
				  (unsigned long)original_path_length,
				  translated_path,
				  (unsigned long)translated_path_length));
			TALLOC_FREE(original_path);
			TALLOC_FREE(translated_path);
			return;
		}

		/* we only want to index by the first part of original_path,
			up to the length of translated_path */

		original_path[translated_path_length] = '\0';
		original_path_length = translated_path_length;
	}

	/* Ensure we're null terminated. */
	translated_path[translated_path_length] = '\0';

	/*
	 * New entry or replace old entry.
	 */

	memcache_add(
		smbd_memcache(), STAT_CACHE,
		data_blob_const(original_path, original_path_length),
		data_blob_const(translated_path, translated_path_length + 1));

	DEBUG(5,("stat_cache_add: Added entry (%lx:size %x) %s -> %s\n",
		 (unsigned long)translated_path,
		 (unsigned int)translated_path_length,
		 original_path,
		 translated_path));

	TALLOC_FREE(original_path);
	TALLOC_FREE(translated_path);
}

/**
 * Look through the stat cache for an entry
 *
 * @param conn    A connection struct to do the stat() with.
 * @param posix_paths Whether to lookup using stat() or lstat()
 * @param name    The path we are attempting to cache, modified by this routine
 *                to be correct as far as the cache can tell us. We assume that
 *		  it is a talloc'ed string from top of stack, we free it if
 *		  necessary.
 * @param dirpath The path as far as the stat cache told us. Also talloced
 * 		  from top of stack.
 * @param start   A pointer into name, for where to 'start' in fixing the rest
 * 		  of the name up.
 * @param psd     A stat buffer, NOT from the cache, but just a side-effect.
 *
 * @return True if we translated (and did a scuccessful stat on) the entire
 * 		  name.
 *
 */

bool stat_cache_lookup(connection_struct *conn,
			char **pp_name,
			char **pp_dirpath,
			char **pp_start,
			NTTIME twrp,
			SMB_STRUCT_STAT *pst)
{
	char *chk_name;
	size_t namelen;
	bool sizechanged = False;
	unsigned int num_components = 0;
	char *translated_path;
	size_t translated_path_length;
	DATA_BLOB data_val;
	char *name;
	TALLOC_CTX *ctx = talloc_tos();
	struct smb_filename smb_fname;
	int ret;

	*pp_dirpath = NULL;
	*pp_start = *pp_name;

	if (!lp_stat_cache()) {
		return False;
	}

	name = *pp_name;
	namelen = strlen(name);

	DO_PROFILE_INC(statcache_lookups);

	/*
	 * Don't lookup trivial valid directory entries.
	 */
	if ((*name == '\0') || ISDOT(name) || ISDOTDOT(name)) {
		return False;
	}

	if (conn->case_sensitive) {
		chk_name = talloc_asprintf(ctx,
					   STAT_CACHE_TWRP_TOKEN,
					   twrp,
					   name);
		if (!chk_name) {
			DEBUG(0, ("stat_cache_lookup: strdup failed!\n"));
			return False;
		}

	} else {
		char *upper = NULL;

		upper = talloc_strdup_upper(ctx,name);
		if (upper == NULL) {
			DBG_ERR("talloc_strdup_upper failed!\n");
			return False;
		}
		chk_name = talloc_asprintf(ctx,
					   STAT_CACHE_TWRP_TOKEN,
					   twrp,
					   upper);
		if (!chk_name) {
			DEBUG(0, ("stat_cache_lookup: talloc_strdup_upper failed!\n"));
			return False;
		}

		/*
		 * In some language encodings the length changes
		 * if we uppercase. We need to treat this differently
		 * below.
		 */
		if (strlen(chk_name) != namelen) {
			sizechanged = True;
		}
	}

	while (1) {
		char *sp;

		data_val = data_blob_null;

		if (memcache_lookup(
			    smbd_memcache(), STAT_CACHE,
			    data_blob_const(chk_name, strlen(chk_name)),
			    &data_val)) {
			break;
		}

		DEBUG(10,("stat_cache_lookup: lookup failed for name [%s]\n",
				chk_name ));
		/*
		 * Didn't find it - remove last component for next try.
		 */
		if (!(sp = strrchr_m(chk_name, '/'))) {
			/*
			 * We reached the end of the name - no match.
			 */
			DO_PROFILE_INC(statcache_misses);
			TALLOC_FREE(chk_name);
			return False;
		}

		*sp = '\0';

		/*
		 * Count the number of times we have done this, we'll
		 * need it when reconstructing the string.
		 */
		num_components++;

		if ((*chk_name == '\0')
		    || ISDOT(chk_name) || ISDOTDOT(chk_name)) {
			DO_PROFILE_INC(statcache_misses);
			TALLOC_FREE(chk_name);
			return False;
		}
	}

	SMB_ASSERT(data_val.length >= STAT_CACHE_TWRP_TOKEN_LEN);

	translated_path = talloc_strdup(
		ctx,(char *)data_val.data + STAT_CACHE_TWRP_TOKEN_LEN);
	if (!translated_path) {
		smb_panic("talloc failed");
	}
	translated_path_length = data_val.length - 1 - STAT_CACHE_TWRP_TOKEN_LEN;

	DEBUG(10,("stat_cache_lookup: lookup succeeded for name [%s] "
		  "-> [%s]\n", chk_name, translated_path ));
	DO_PROFILE_INC(statcache_hits);

	smb_fname = (struct smb_filename) {
		.base_name = translated_path,
		.twrp = twrp,
	};

	ret = vfs_stat(conn, &smb_fname);
	if (ret != 0) {
		/* Discard this entry - it doesn't exist in the filesystem. */
		memcache_delete(smbd_memcache(), STAT_CACHE,
				data_blob_const(chk_name, strlen(chk_name)));
		TALLOC_FREE(chk_name);
		TALLOC_FREE(translated_path);
		return False;
	}
	/*
	 * Only copy the stat struct back if we actually hit the full path
	 */
	if (num_components == 0) {
		*pst = smb_fname.st;
	}

	if (!sizechanged) {
		memcpy(*pp_name, translated_path,
		       MIN(namelen, translated_path_length));
	} else {
		if (num_components == 0) {
			name = talloc_strndup(ctx, translated_path,
					   translated_path_length);
		} else {
			char *sp;

			sp = strnrchr_m(name, '/', num_components);
			if (sp) {
				name = talloc_asprintf(ctx,"%.*s%s",
					 (int)translated_path_length,
					 translated_path, sp);
			} else {
				name = talloc_strndup(ctx,
						translated_path,
						translated_path_length);
			}
		}
		if (name == NULL) {
			/*
			 * TODO: Get us out of here with a real error message
			 */
			smb_panic("talloc failed");
		}
		TALLOC_FREE(*pp_name);
		*pp_name = name;
	}


	/* set pointer for 'where to start' on fixing the rest of the name */
	*pp_start = &name[translated_path_length];
	if (**pp_start == '/') {
		++*pp_start;
	}

	*pp_dirpath = translated_path;
	TALLOC_FREE(chk_name);
	return (namelen == translated_path_length);
}

/***************************************************************************
 Tell all smbd's to delete an entry.
**************************************************************************/

void smbd_send_stat_cache_delete_message(struct messaging_context *msg_ctx,
					 const char *name)
{
#ifdef DEVELOPER
	messaging_send_all(msg_ctx,
			   MSG_SMB_STAT_CACHE_DELETE,
			   name,
			   strlen(name)+1);
#endif
}

/***************************************************************************
 Delete an entry.
**************************************************************************/

void stat_cache_delete(const char *name)
{
	char *upper = talloc_strdup_upper(talloc_tos(), name);
	char *lname = NULL;

	if (upper == NULL) {
		return;
	}

	lname = talloc_asprintf(talloc_tos(),
				STAT_CACHE_TWRP_TOKEN,
				(uint64_t)0,
				upper);
	TALLOC_FREE(upper);
	if (lname == NULL) {
		return;
	}
	DEBUG(10,("stat_cache_delete: deleting name [%s] -> %s\n",
			lname, name ));

	memcache_delete(smbd_memcache(), STAT_CACHE,
			data_blob_const(lname, talloc_get_size(lname)-1));
	TALLOC_FREE(lname);
}

/***************************************************************************
 Initializes or clears the stat cache.
**************************************************************************/

bool reset_stat_cache( void )
{
	if (!lp_stat_cache())
		return True;

	memcache_flush(smbd_memcache(), STAT_CACHE);

	return True;
}
