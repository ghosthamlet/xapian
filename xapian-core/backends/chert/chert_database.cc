/* chert_database.cc: chert database
 *
 * Copyright 1999,2000,2001 BrightStation PLC
 * Copyright 2001 Hein Ragas
 * Copyright 2002 Ananova Ltd
 * Copyright 2002,2003,2004,2005,2006,2007,2008,2009 Olly Betts
 * Copyright 2006,2008 Lemur Consulting Ltd
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301
 * USA
 */

#include <config.h>

#include "chert_database.h"

#include <xapian/error.h>
#include <xapian/replication.h>
#include <xapian/valueiterator.h>

#include "contiguousalldocspostlist.h"
#include "chert_alldocsmodifiedpostlist.h"
#include "chert_alldocspostlist.h"
#include "chert_alltermslist.h"
#include "chert_replicate_internal.h"
#include "chert_document.h"
#include "chert_io.h"
#include "chert_lock.h"
#include "chert_metadata.h"
#include "chert_modifiedpostlist.h"
#include "chert_positionlist.h"
#include "chert_postlist.h"
#include "chert_record.h"
#include "chert_spellingwordslist.h"
#include "chert_termlist.h"
#include "chert_valuelist.h"
#include "chert_values.h"
#include "omdebug.h"
#include "omtime.h"
#include "pack.h"
#include "remoteconnection.h"
#include "replicationprotocol.h"
#include "serialise.h"
#include "stringutils.h"
#include "utils.h"
#include "valuestats.h"

#ifdef __WIN32__
# include "msvc_posix_wrapper.h"
#endif

#include "safeerrno.h"
#include "safesysstat.h"
#include <sys/types.h>

#include <algorithm>
#include "autoptr.h"
#include <string>

using namespace std;
using namespace Xapian;

// The maximum safe term length is determined by the postlist.  There we
// store the term followed by "\x00\x00" then a length byte, then up to
// 4 bytes of docid.  The Btree manager's key length limit is 252 bytes
// so the maximum safe term length is 252 - 2 - 1 - 4 = 245 bytes.  If
// the term contains zero bytes, the limit is lower (by one for each zero byte
// in the term).
#define MAX_SAFE_TERM_LENGTH 245

/** Delete file, throwing an error if we can't delete it (but not if it
 *  doesn't exist).
 */
static void
sys_unlink_if_exists(const string & filename)
{
#ifdef __WIN32__
    if (msvc_posix_unlink(filename.c_str()) == -1) {
#else
    if (unlink(filename) == -1) {
#endif
	if (errno == ENOENT) return;
	throw Xapian::DatabaseError("Can't delete file: `" + filename + "'",
				    errno);
    }
}

/* This finds the tables, opens them at consistent revisions, manages
 * determining the current and next revision numbers, and stores handles
 * to the tables.
 */
ChertDatabase::ChertDatabase(const string &chert_dir, int action,
			     unsigned int block_size)
	: db_dir(chert_dir),
	  readonly(action == XAPIAN_DB_READONLY),
	  version_file(db_dir),
	  postlist_table(db_dir, readonly),
	  position_table(db_dir, readonly),
	  termlist_table(db_dir, readonly),
	  value_manager(&postlist_table, &termlist_table),
	  synonym_table(db_dir, readonly),
	  spelling_table(db_dir, readonly),
	  record_table(db_dir, readonly),
	  // Keep the same lockfile name as flint since the locking is
	  // compatible and this avoids the possibility of creating a chert and
	  // flint database in the same directory (which will result in one
	  // being corrupt since the Btree filenames overlap).
	  lock(db_dir + "/flintlock"),
	  max_changesets(0)
{
    DEBUGCALL(DB, void, "ChertDatabase", chert_dir << ", " << action <<
	      ", " << block_size);

    if (action == XAPIAN_DB_READONLY) {
	open_tables_consistent();
	return;
    }

    const char *p = getenv("XAPIAN_MAX_CHANGESETS");
    if (p)
	max_changesets = atoi(p);

    if (action != Xapian::DB_OPEN && !database_exists()) {
	// FIXME: if we allow Xapian::DB_OVERWRITE, check it here

	// Create the directory for the database, if it doesn't exist
	// already.
	bool fail = false;
	struct stat statbuf;
	if (stat(db_dir, &statbuf) == 0) {
	    if (!S_ISDIR(statbuf.st_mode)) fail = true;
	} else if (errno != ENOENT || mkdir(db_dir, 0755) == -1) {
	    fail = true;
	}
	if (fail) {
	    throw Xapian::DatabaseCreateError("Cannot create directory `" +
					      db_dir + "'", errno);
	}
	get_database_write_lock(true);

	create_and_open_tables(block_size);
	return;
    }

    if (action == Xapian::DB_CREATE) {
	throw Xapian::DatabaseCreateError("Can't create new database at `" +
					  db_dir + "': a database already exists and I was told "
					  "not to overwrite it");
    }

    get_database_write_lock(false);
    // if we're overwriting, pretend the db doesn't exist
    // FIXME: if we allow Xapian::DB_OVERWRITE, check it here
    if (action == Xapian::DB_CREATE_OR_OVERWRITE) {
	create_and_open_tables(block_size);
	return;
    }

    // Get latest consistent version
    open_tables_consistent();

    // Check that there are no more recent versions of tables.  If there
    // are, perform recovery by writing a new revision number to all
    // tables.
    if (record_table.get_open_revision_number() !=
	postlist_table.get_latest_revision_number()) {
	chert_revision_number_t new_revision = get_next_revision_number();

	set_revision_number(new_revision);
    }
}

ChertDatabase::~ChertDatabase()
{
    DEBUGCALL(DB, void, "~ChertDatabase", "");
}

bool
ChertDatabase::database_exists() {
    DEBUGCALL(DB, bool, "ChertDatabase::database_exists", "");
    RETURN(record_table.exists() && postlist_table.exists());
}

void
ChertDatabase::create_and_open_tables(unsigned int block_size)
{
    DEBUGCALL(DB, void, "ChertDatabase::create_and_open_tables", "");
    // The caller is expected to create the database directory if it doesn't
    // already exist.

    // Create postlist_table first, and record_table last.  Existence of
    // record_table is considered to imply existence of the database.
    version_file.create();
    postlist_table.create_and_open(block_size);
    position_table.create_and_open(block_size);
    termlist_table.create_and_open(block_size);
    synonym_table.create_and_open(block_size);
    spelling_table.create_and_open(block_size);
    record_table.create_and_open(block_size);

    Assert(database_exists());

    // Check consistency
    chert_revision_number_t revision = record_table.get_open_revision_number();
    if (revision != postlist_table.get_open_revision_number()) {
	throw Xapian::DatabaseCreateError("Newly created tables are not in consistent state");
    }

    stats.zero();
}

void
ChertDatabase::open_tables_consistent()
{
    DEBUGCALL(DB, void, "ChertDatabase::open_tables_consistent", "");
    // Open record_table first, since it's the last to be written to,
    // and hence if a revision is available in it, it should be available
    // in all the other tables (unless they've moved on already).
    //
    // If we find that a table can't open the desired revision, we
    // go back and open record_table again, until record_table has
    // the same revision as the last time we opened it.

    chert_revision_number_t cur_rev = record_table.get_open_revision_number();

    // Check the version file unless we're reopening.
    if (cur_rev == 0) version_file.read_and_check();

    record_table.open();
    chert_revision_number_t revision = record_table.get_open_revision_number();

    if (cur_rev && cur_rev == revision) {
	// We're reopening a database and the revision hasn't changed so we
	// don't need to do anything.
	return;
    }

    // Set the block_size for optional tables as they may not currently exist.
    unsigned int block_size = record_table.get_block_size();
    position_table.set_block_size(block_size);
    termlist_table.set_block_size(block_size);
    synonym_table.set_block_size(block_size);
    spelling_table.set_block_size(block_size);

    value_manager.reset();

    bool fully_opened = false;
    int tries = 100;
    int tries_left = tries;
    while (!fully_opened && (tries_left--) > 0) {
	if (spelling_table.open(revision) &&
	    synonym_table.open(revision) &&
	    termlist_table.open(revision) &&
	    position_table.open(revision) &&
	    postlist_table.open(revision)) {
	    // Everything now open at the same revision.
	    fully_opened = true;
	} else {
	    // Couldn't open consistent revision: two cases possible:
	    // i)   An update has completed and a second one has begun since
	    //      record was opened.  This leaves a consistent revision
	    //      available, but not the one we were trying to open.
	    // ii)  Tables have become corrupt / have no consistent revision
	    //      available.  In this case, updates must have ceased.
	    //
	    // So, we reopen the record table, and check its revision number,
	    // if it's changed we try the opening again, otherwise we give up.
	    //
	    record_table.open();
	    chert_revision_number_t newrevision =
		    record_table.get_open_revision_number();
	    if (revision == newrevision) {
		// Revision number hasn't changed - therefore a second index
		// sweep hasn't begun and the system must have failed.  Database
		// is inconsistent.
		throw Xapian::DatabaseCorruptError("Cannot open tables at consistent revisions");
	    }
	    revision = newrevision;
	}
    }

    if (!fully_opened) {
	throw Xapian::DatabaseModifiedError("Cannot open tables at stable revision - changing too fast");
    }

    stats.read(postlist_table);
}

void
ChertDatabase::open_tables(chert_revision_number_t revision)
{
    DEBUGCALL(DB, void, "ChertDatabase::open_tables", revision);
    version_file.read_and_check();
    record_table.open(revision);

    // Set the block_size for optional tables as they may not currently exist.
    unsigned int block_size = record_table.get_block_size();
    position_table.set_block_size(block_size);
    termlist_table.set_block_size(block_size);
    synonym_table.set_block_size(block_size);
    spelling_table.set_block_size(block_size);

    value_manager.reset();

    spelling_table.open(revision);
    synonym_table.open(revision);
    termlist_table.open(revision);
    position_table.open(revision);
    postlist_table.open(revision);
}

chert_revision_number_t
ChertDatabase::get_revision_number() const
{
    DEBUGCALL(DB, chert_revision_number_t, "ChertDatabase::get_revision_number", "");
    // We could use any table here, theoretically.
    RETURN(postlist_table.get_open_revision_number());
}

chert_revision_number_t
ChertDatabase::get_next_revision_number() const
{
    DEBUGCALL(DB, chert_revision_number_t, "ChertDatabase::get_next_revision_number", "");
    /* We _must_ use postlist_table here, since it is always the first
     * to be written, and hence will have the greatest available revision
     * number.
     */
    chert_revision_number_t new_revision =
	    postlist_table.get_latest_revision_number();
    ++new_revision;
    RETURN(new_revision);
}

void
ChertDatabase::get_changeset_revisions(const string & path,
				       chert_revision_number_t * startrev,
				       chert_revision_number_t * endrev) const
{
    int changes_fd = -1;
#ifdef __WIN32__
    changes_fd = msvc_posix_open(path.c_str(), O_RDONLY);
#else
    changes_fd = open(path.c_str(), O_RDONLY);
#endif
    fdcloser closer(changes_fd);

    if (changes_fd < 0) {
	string message = string("Couldn't open changeset ")
		+ path + " to read";
	throw Xapian::DatabaseError(message, errno);
    }

    char buf[REASONABLE_CHANGESET_SIZE];
    const char *start = buf;
    const char *end = buf + chert_io_read(changes_fd, buf,
					  REASONABLE_CHANGESET_SIZE, 0);
    if (strncmp(start, CHANGES_MAGIC_STRING,
		CONST_STRLEN(CHANGES_MAGIC_STRING)) != 0) {
	string message = string("Changeset at ")
		+ path + " does not contain valid magic string";
	throw Xapian::DatabaseError(message);
    }
    start += CONST_STRLEN(CHANGES_MAGIC_STRING);
    if (start >= end)
	throw Xapian::DatabaseError("Changeset too short at " + path);

    unsigned int changes_version;
    if (!unpack_uint(&start, end, &changes_version))
	throw Xapian::DatabaseError("Couldn't read a valid version number for "
				    "changeset at " + path);
    if (changes_version != CHANGES_VERSION)
	throw Xapian::DatabaseError("Don't support version of changeset at "
				    + path);

    if (!unpack_uint(&start, end, startrev))
	throw Xapian::DatabaseError("Couldn't read a valid start revision from "
				    "changeset at " + path);

    if (!unpack_uint(&start, end, endrev))
	throw Xapian::DatabaseError("Couldn't read a valid end revision for "
				    "changeset at " + path);
}

void
ChertDatabase::set_revision_number(chert_revision_number_t new_revision)
{
    DEBUGCALL(DB, void, "ChertDatabase::set_revision_number", new_revision);

    value_manager.merge_changes();

    postlist_table.flush_db();
    position_table.flush_db();
    termlist_table.flush_db();
    synonym_table.flush_db();
    spelling_table.flush_db();
    record_table.flush_db();

    int changes_fd = -1;
    string changes_name;

    if (max_changesets > 0) {
	chert_revision_number_t old_revision = get_revision_number();
	if (old_revision) {
	    // Don't generate a changeset for the first revision.
	    changes_name = db_dir + "/changes" + om_tostring(old_revision);
#ifdef __WIN32__
	    changes_fd = msvc_posix_open(changes_name.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_BINARY);
#else
	    changes_fd = open(changes_name.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666);
#endif
	    if (changes_fd < 0) {
		string message = string("Couldn't open changeset ")
			+ changes_name + " to write";
		throw Xapian::DatabaseError(message, errno);
	    }
	}
    }

    try {
	fdcloser closefd(changes_fd);
	if (changes_fd >= 0) {
	    string buf;
	    chert_revision_number_t old_revision = get_revision_number();
	    buf += CHANGES_MAGIC_STRING;
	    pack_uint(buf, CHANGES_VERSION);
	    pack_uint(buf, old_revision);
	    pack_uint(buf, new_revision);

	    // FIXME - if DANGEROUS mode is in use, this should be 1 not 0.
	    pack_uint(buf, 0u); // Changes can be applied to a live database.

	    chert_io_write(changes_fd, buf.data(), buf.size());

	    // Write the changes to the blocks in the tables.  Do the postlist
	    // table last, so that ends up cached the most, if the cache
	    // available is limited.  Do the position table just before that
	    // as having that cached will also improve search performance.
	    termlist_table.write_changed_blocks(changes_fd);
	    synonym_table.write_changed_blocks(changes_fd);
	    spelling_table.write_changed_blocks(changes_fd);
	    record_table.write_changed_blocks(changes_fd);
	    position_table.write_changed_blocks(changes_fd);
	    postlist_table.write_changed_blocks(changes_fd);
	}

	postlist_table.commit(new_revision, changes_fd);
	position_table.commit(new_revision, changes_fd);
	termlist_table.commit(new_revision, changes_fd);
	synonym_table.commit(new_revision, changes_fd);
	spelling_table.commit(new_revision, changes_fd);

	string changes_tail; // Data to be appended to the changes file
	if (changes_fd >= 0) {
	    changes_tail += '\0';
	    pack_uint(changes_tail, new_revision);
	}
	record_table.commit(new_revision, changes_fd, &changes_tail);

    } catch (...) {
	// Remove the changeset, if there was one.
	if (changes_fd >= 0) {
	    sys_unlink_if_exists(changes_name);
	}

	throw;
    }
}

void
ChertDatabase::reopen()
{
    DEBUGCALL(DB, void, "ChertDatabase::reopen", "");
    if (readonly) open_tables_consistent();
}

void
ChertDatabase::close()
{
    DEBUGCALL(DB, void, "ChertDatabase::close", "");
    postlist_table.close(true);
    position_table.close(true);
    termlist_table.close(true);
    synonym_table.close(true);
    spelling_table.close(true);
    record_table.close(true);
    lock.release();
}

void
ChertDatabase::get_database_write_lock(bool creating)
{
    DEBUGCALL(DB, void, "ChertDatabase::get_database_write_lock", creating);
    string explanation;
    ChertLock::reason why = lock.lock(true, explanation);
    if (why != ChertLock::SUCCESS) {
	if (why == ChertLock::UNKNOWN && !creating && !database_exists()) {
	    string msg("No chert database found at path `");
	    msg += db_dir;
	    msg += '\'';
	    throw Xapian::DatabaseOpeningError(msg);
	}
	string msg("Unable to acquire database write lock on ");
	msg += db_dir;
	if (why == ChertLock::INUSE) {
	    msg += ": already locked";
	} else if (why == ChertLock::UNSUPPORTED) {
	    msg += ": locking probably not supported by this FS";
	} else if (why == ChertLock::FDLIMIT) {
	    msg += ": too many open files";
	} else if (why == ChertLock::UNKNOWN) {
	    if (!explanation.empty())
		msg += ": " + explanation;
	}
	throw Xapian::DatabaseLockError(msg);
    }
}

void
ChertDatabase::send_whole_database(RemoteConnection & conn,
				   const OmTime & end_time)
{
    DEBUGCALL(DB, void, "ChertDatabase::send_whole_database",
	      "conn" << ", " << "end_time");

    // Send the current revision number in the header.
    string buf;
    string uuid = get_uuid();
    buf += encode_length(uuid.size());
    buf += uuid;
    pack_uint(buf, get_revision_number());
    conn.send_message(REPL_REPLY_DB_HEADER, buf, end_time);

    // Send all the tables.  The tables which we want to be cached best after
    // the copy finished are sent last.
    static const char filenames[] =
	"\x0b""termlist.DB""\x0e""termlist.baseA\x0e""termlist.baseB"
	"\x0a""synonym.DB""\x0d""synonym.baseA\x0d""synonym.baseB"
	"\x0b""spelling.DB""\x0e""spelling.baseA\x0e""spelling.baseB"
	"\x09""record.DB""\x0c""record.baseA\x0c""record.baseB"
	"\x0b""position.DB""\x0e""position.baseA\x0e""position.baseB"
	"\x0b""postlist.DB""\x0e""postlist.baseA\x0e""postlist.baseB"
	"\x08""iamchert";
    string filepath = db_dir;
    filepath += '/';
    for (const char * p = filenames; *p; p += *p + 1) {
	string leaf(p + 1, size_t(static_cast<unsigned char>(*p)));
        filepath.replace(db_dir.size() + 1, string::npos, leaf);
	if (file_exists(filepath)) {
	    // FIXME - there is a race condition here - the file might get
	    // deleted between the file_exists() test and the access to send it.
	    conn.send_message(REPL_REPLY_DB_FILENAME, leaf, end_time);
	    conn.send_file(REPL_REPLY_DB_FILEDATA, filepath, end_time);
	}
    }
}

void
ChertDatabase::write_changesets_to_fd(int fd,
				      const string & revision,
				      bool need_whole_db,
				      ReplicationInfo * info)
{
    DEBUGCALL(DB, void, "ChertDatabase::write_changesets_to_fd",
	      fd << ", " << revision << ", " << need_whole_db << ", " << info);

    int whole_db_copies_left = MAX_DB_COPIES_PER_CONVERSATION;
    chert_revision_number_t start_rev_num = 0;
    string start_uuid = get_uuid();

    chert_revision_number_t needed_rev_num = 0;

    const char * rev_ptr = revision.data();
    const char * rev_end = rev_ptr + revision.size();
    if (!unpack_uint(&rev_ptr, rev_end, &start_rev_num)) {
	need_whole_db = true;
    }

    RemoteConnection conn(-1, fd, string());
    OmTime end_time;

    // While the starting revision number is less than the latest revision
    // number, look for a changeset, and write it.
    //
    // FIXME - perhaps we should make hardlinks for all the changesets we're
    // likely to need, first, and then start sending them, so that there's no
    // risk of them disappearing while we're sending earlier ones.
    while (true) {
	if (need_whole_db) {
	    // Decrease the counter of copies left to be sent, and fail
	    // if we've already copied the database enough.  This ensures that
	    // synchronisation attempts always terminate eventually.
	    if (whole_db_copies_left == 0) {
		conn.send_message(REPL_REPLY_FAIL,
				  "Database changing too fast",
				  end_time);
		return;
	    }
	    whole_db_copies_left--;

	    // Send the whole database across.
	    start_rev_num = get_revision_number();
	    start_uuid = get_uuid();

	    send_whole_database(conn, end_time);
	    if (info != NULL)
		++(info->fullcopy_count);

	    need_whole_db = false;

	    reopen();
	    if (start_uuid == get_uuid()) {
		// Send the latest revision number after sending the tables.
		// The update must proceed to that revision number before the
		// copy is safe to make live.

		string buf;
		needed_rev_num = get_revision_number();
		pack_uint(buf, needed_rev_num);
		conn.send_message(REPL_REPLY_DB_FOOTER, buf, end_time);
		if (info != NULL && start_rev_num == needed_rev_num)
		    info->changed = true;
	    } else {
		// Database has been replaced since we did the copy.  Send a
		// higher revision number than the revision we've just copied,
		// so that the client doesn't make the copy we've just done
		// live, and then mark that we need to do a copy again.
		// The client will never actually get the required revision,
		// because the next message is going to be the start of a new
		// database transfer.

		string buf;
		pack_uint(buf, start_rev_num + 1);
		conn.send_message(REPL_REPLY_DB_FOOTER, buf, end_time);
		need_whole_db = true;
	    }
	} else {
	    // Check if we've sent all the updates.
	    if (start_rev_num >= get_revision_number()) {
		reopen();
		if (start_uuid != get_uuid()) {
		    need_whole_db = true;
		    continue;
		}
		if (start_rev_num >= get_revision_number()) {
		    break;
		}
	    }

	    // Look for the changeset for revision start_rev_num.
	    string changes_name = db_dir + "/changes" + om_tostring(start_rev_num);
	    if (file_exists(changes_name)) {
		// Send it, and also update start_rev_num to the new value
		// specified in the changeset.
		chert_revision_number_t changeset_start_rev_num;
		chert_revision_number_t changeset_end_rev_num;
		get_changeset_revisions(changes_name,
					&changeset_start_rev_num,
					&changeset_end_rev_num);
		if (changeset_start_rev_num != start_rev_num) {
		    throw Xapian::DatabaseError("Changeset start revision does not match changeset filename");
		}
		if (changeset_start_rev_num >= changeset_end_rev_num) {
		    throw Xapian::DatabaseError("Changeset start revision is not less than end revision");
		}
		// FIXME - there is a race condition here - the file might get
		// deleted between the file_exists() test and the access to
		// send it.
		conn.send_file(REPL_REPLY_CHANGESET, changes_name, end_time);
		start_rev_num = changeset_end_rev_num;
		if (info != NULL) {
		    ++(info->changeset_count);
		    if (start_rev_num >= needed_rev_num)
			info->changed = true;
		}
	    } else {
		// The changeset doesn't exist: leave the revision number as it
		// is, and mark for doing a full database copy.
		need_whole_db = true;
	    }
	}
    }
    conn.send_message(REPL_REPLY_END_OF_CHANGES, string(), end_time);
}

void
ChertDatabase::modifications_failed(chert_revision_number_t old_revision,
				    chert_revision_number_t new_revision,
				    const std::string & msg)
{
    // Modifications failed.  Wipe all the modifications from memory.
    try {
	// Discard any buffered changes and reinitialised cached values
	// from the table.
	cancel();

	// Reopen tables with old revision number.
	open_tables(old_revision);

	// Increase revision numbers to new revision number plus one,
	// writing increased numbers to all tables.
	++new_revision;
	set_revision_number(new_revision);
    } catch (const Xapian::Error &e) {
	// We can't get the database into a consistent state, so close
	// it to avoid the risk of database corruption.
	ChertDatabase::close();
	throw Xapian::DatabaseError("Modifications failed (" + msg +
				    "), and cannot set consistent table "
				    "revision numbers: " + e.get_msg());
    }
}

void
ChertDatabase::apply()
{
    DEBUGCALL(DB, void, "ChertDatabase::apply", "");
    if (!postlist_table.is_modified() &&
	!position_table.is_modified() &&
	!termlist_table.is_modified() &&
	!value_manager.is_modified() &&
	!synonym_table.is_modified() &&
	!spelling_table.is_modified() &&
	!record_table.is_modified()) {
	return;
    }

    chert_revision_number_t old_revision = get_revision_number();
    chert_revision_number_t new_revision = get_next_revision_number();

    try {
	set_revision_number(new_revision);
    } catch (const Xapian::Error &e) {
	modifications_failed(old_revision, new_revision, e.get_description());
	throw;
    } catch (...) {
	modifications_failed(old_revision, new_revision, "Unknown error");
	throw;
    }
}

void
ChertDatabase::cancel()
{
    DEBUGCALL(DB, void, "ChertDatabase::cancel", "");
    postlist_table.cancel();
    position_table.cancel();
    termlist_table.cancel();
    value_manager.cancel();
    synonym_table.cancel();
    spelling_table.cancel();
    record_table.cancel();
}

Xapian::doccount
ChertDatabase::get_doccount() const
{
    DEBUGCALL(DB, Xapian::doccount, "ChertDatabase::get_doccount", "");
    RETURN(record_table.get_doccount());
}

Xapian::docid
ChertDatabase::get_lastdocid() const
{
    DEBUGCALL(DB, Xapian::docid, "ChertDatabase::get_lastdocid", "");
    RETURN(stats.get_last_docid());
}

totlen_t
ChertDatabase::get_total_length() const
{
    DEBUGCALL(DB, totlen_t, "ChertDatabase::get_total_length", "");
    RETURN(stats.get_total_doclen());
}

Xapian::doclength
ChertDatabase::get_avlength() const
{
    DEBUGCALL(DB, Xapian::doclength, "ChertDatabase::get_avlength", "");
    Xapian::doccount doccount = record_table.get_doccount();
    if (doccount == 0) {
	// Avoid dividing by zero when there are no documents.
	RETURN(0);
    }
    RETURN(double(stats.get_total_doclen()) / doccount);
}

Xapian::termcount
ChertDatabase::get_doclength(Xapian::docid did) const
{
    DEBUGCALL(DB, Xapian::termcount, "ChertDatabase::get_doclength", did);
    Assert(did != 0);
    Xapian::Internal::RefCntPtr<const ChertDatabase> ptrtothis(this);
    RETURN(postlist_table.get_doclength(did, ptrtothis));
}

Xapian::doccount
ChertDatabase::get_termfreq(const string & term) const
{
    DEBUGCALL(DB, Xapian::doccount, "ChertDatabase::get_termfreq", term);
    Assert(!term.empty());
    RETURN(postlist_table.get_termfreq(term));
}

Xapian::termcount
ChertDatabase::get_collection_freq(const string & term) const
{
    DEBUGCALL(DB, Xapian::termcount, "ChertDatabase::get_collection_freq", term);
    Assert(!term.empty());
    RETURN(postlist_table.get_collection_freq(term));
}

Xapian::doccount
ChertDatabase::get_value_freq(Xapian::valueno valno) const
{
    DEBUGCALL(DB, Xapian::doccount, "ChertDatabase::get_value_freq", valno);
    RETURN(value_manager.get_value_freq(valno));
}

std::string
ChertDatabase::get_value_lower_bound(Xapian::valueno valno) const
{
    DEBUGCALL(DB, std::string, "ChertDatabase::get_value_lower_bound", valno);
    RETURN(value_manager.get_value_lower_bound(valno));
}

std::string
ChertDatabase::get_value_upper_bound(Xapian::valueno valno) const
{
    DEBUGCALL(DB, std::string, "ChertDatabase::get_value_upper_bound", valno);
    RETURN(value_manager.get_value_upper_bound(valno));
}

Xapian::termcount
ChertDatabase::get_doclength_lower_bound() const
{
    return stats.get_doclength_lower_bound();
}

Xapian::termcount
ChertDatabase::get_doclength_upper_bound() const
{
    return stats.get_doclength_upper_bound();
}

Xapian::termcount
ChertDatabase::get_wdf_upper_bound(const string & term) const
{
    return min(get_collection_freq(term), stats.get_wdf_upper_bound());
}

bool
ChertDatabase::term_exists(const string & term) const
{
    DEBUGCALL(DB, bool, "ChertDatabase::term_exists", term);
    Assert(!term.empty());
    return postlist_table.term_exists(term);
}

bool
ChertDatabase::has_positions() const
{
    return position_table.get_entry_count() > 0;
}

LeafPostList *
ChertDatabase::open_post_list(const string& term) const
{
    DEBUGCALL(DB, LeafPostList *, "ChertDatabase::open_post_list", term);
    Xapian::Internal::RefCntPtr<const ChertDatabase> ptrtothis(this);

    if (term.empty()) {
	Xapian::doccount doccount = get_doccount();
	if (stats.get_last_docid() == doccount) {
	    RETURN(new ContiguousAllDocsPostList(ptrtothis, doccount));
	}
	RETURN(new ChertAllDocsPostList(ptrtothis, doccount));
    }

    RETURN(new ChertPostList(ptrtothis, term, true));
}

ValueList *
ChertDatabase::open_value_list(Xapian::valueno slot) const
{
    DEBUGCALL(DB, ValueList *, "ChertDatabase::open_value_list", slot);
    Xapian::Internal::RefCntPtr<const ChertDatabase> ptrtothis(this);
    RETURN(new ChertValueList(slot, ptrtothis));
}

TermList *
ChertDatabase::open_term_list(Xapian::docid did) const
{
    DEBUGCALL(DB, TermList *, "ChertDatabase::open_term_list", did);
    Assert(did != 0);
    if (!termlist_table.is_open())
	throw Xapian::FeatureUnavailableError("Database has no termlist");

    Xapian::Internal::RefCntPtr<const ChertDatabase> ptrtothis(this);
    RETURN(new ChertTermList(ptrtothis, did));
}

Xapian::Document::Internal *
ChertDatabase::open_document(Xapian::docid did, bool lazy) const
{
    DEBUGCALL(DB, Xapian::Document::Internal *, "ChertDatabase::open_document",
	      did << ", " << lazy);
    Assert(did != 0);
    if (!lazy) {
	// This will throw DocNotFoundError if the document doesn't exist.
	(void)get_doclength(did);
    }

    Xapian::Internal::RefCntPtr<const Database::Internal> ptrtothis(this);
    RETURN(new ChertDocument(ptrtothis, did, &value_manager, &record_table));
}

PositionList *
ChertDatabase::open_position_list(Xapian::docid did, const string & term) const
{
    Assert(did != 0);

    AutoPtr<ChertPositionList> poslist(new ChertPositionList);
    if (!poslist->read_data(&position_table, did, term)) {
	// As of 1.1.0, we don't check if the did and term exist - we just
	// return an empty positionlist.  If the user really needs to know,
	// they can check for themselves.
    }

    return poslist.release();
}

TermList *
ChertDatabase::open_allterms(const string & prefix) const
{
    DEBUGCALL(DB, TermList *, "ChertDatabase::open_allterms", "");
    RETURN(new ChertAllTermsList(Xapian::Internal::RefCntPtr<const ChertDatabase>(this),
				 prefix));
}

TermList *
ChertDatabase::open_spelling_termlist(const string & word) const
{
    return spelling_table.open_termlist(word);
}

TermList *
ChertDatabase::open_spelling_wordlist() const
{
    ChertCursor * cursor = spelling_table.cursor_get();
    if (!cursor) return NULL;
    return new ChertSpellingWordsList(Xapian::Internal::RefCntPtr<const ChertDatabase>(this),
				      cursor);
}

Xapian::doccount
ChertDatabase::get_spelling_frequency(const string & word) const
{
    return spelling_table.get_word_frequency(word);
}

TermList *
ChertDatabase::open_synonym_termlist(const string & term) const
{
    return synonym_table.open_termlist(term);
}

TermList *
ChertDatabase::open_synonym_keylist(const string & prefix) const
{
    ChertCursor * cursor = synonym_table.cursor_get();
    if (!cursor) return NULL;
    return new ChertSynonymTermList(Xapian::Internal::RefCntPtr<const ChertDatabase>(this),
				    cursor, synonym_table.get_entry_count(),
				    prefix);
}

string
ChertDatabase::get_metadata(const string & key) const
{
    DEBUGCALL(DB, string, "ChertDatabase::get_metadata", key);
    string btree_key("\x00\xc0", 2);
    btree_key += key;
    string tag;
    (void)postlist_table.get_exact_entry(btree_key, tag);
    RETURN(tag);
}

TermList *
ChertDatabase::open_metadata_keylist(const std::string &prefix) const
{
    DEBUGCALL(DB, string, "ChertDatabase::open_metadata_keylist", "");
    ChertCursor * cursor = postlist_table.cursor_get();
    if (!cursor) return NULL;
    return new ChertMetadataTermList(Xapian::Internal::RefCntPtr<const ChertDatabase>(this),
				     cursor, prefix);
}

string
ChertDatabase::get_revision_info() const
{
    DEBUGCALL(DB, string, "ChertDatabase::get_revision_info", "");
    string buf;
    pack_uint(buf, get_revision_number());
    RETURN(buf);
}

string
ChertDatabase::get_uuid() const
{
    DEBUGCALL(DB, string, "ChertDatabase::get_uuid", "");
    RETURN(version_file.get_uuid_string());
}

///////////////////////////////////////////////////////////////////////////

ChertWritableDatabase::ChertWritableDatabase(const string &dir, int action,
					       int block_size)
	: ChertDatabase(dir, action, block_size),
	  freq_deltas(),
	  doclens(),
	  mod_plists(),
	  change_count(0),
	  flush_threshold(0),
	  modify_shortcut_document(NULL),
	  modify_shortcut_docid(0)
{
    DEBUGCALL(DB, void, "ChertWritableDatabase", dir << ", " << action << ", "
	      << block_size);

    const char *p = getenv("XAPIAN_FLUSH_THRESHOLD");
    if (p)
	flush_threshold = atoi(p);
    if (flush_threshold == 0)
	flush_threshold = 10000;
}

ChertWritableDatabase::~ChertWritableDatabase()
{
    DEBUGCALL(DB, void, "~ChertWritableDatabase", "");
    dtor_called();
}

void
ChertWritableDatabase::commit()
{
    if (transaction_active())
	throw Xapian::InvalidOperationError("Can't commit during a transaction");
    if (change_count) flush_postlist_changes();
    apply();
}

void
ChertWritableDatabase::flush_postlist_changes() const
{
    postlist_table.merge_changes(mod_plists, doclens, freq_deltas);
    stats.write(postlist_table);

    freq_deltas.clear();
    doclens.clear();
    mod_plists.clear();
    change_count = 0;
}

void
ChertWritableDatabase::apply()
{
    value_manager.set_value_stats(value_stats);
    ChertDatabase::apply();
}

Xapian::docid
ChertWritableDatabase::add_document(const Xapian::Document & document)
{
    DEBUGCALL(DB, Xapian::docid,
	      "ChertWritableDatabase::add_document", document);
    // Make sure the docid counter doesn't overflow.
    if (stats.get_last_docid() == Xapian::docid(-1))
	throw Xapian::DatabaseError("Run out of docids - you'll have to use copydatabase to eliminate any gaps before you can add more documents");
    // Use the next unused document ID.
    RETURN(add_document_(stats.get_next_docid(), document));
}

Xapian::docid
ChertWritableDatabase::add_document_(Xapian::docid did,
				     const Xapian::Document & document)
{
    DEBUGCALL(DB, Xapian::docid,
	      "ChertWritableDatabase::add_document_", did << ", " << document);
    Assert(did != 0);
    try {
	// Add the record using that document ID.
	record_table.replace_record(document.get_data(), did);

	// Set the values.
	value_manager.add_document(did, document, value_stats);

	chert_doclen_t new_doclen = 0;
	{
	    Xapian::TermIterator term = document.termlist_begin();
	    Xapian::TermIterator term_end = document.termlist_end();
	    for ( ; term != term_end; ++term) {
		termcount wdf = term.get_wdf();
		// Calculate the new document length
		new_doclen += wdf;
		stats.check_wdf(wdf);

		string tname = *term;
		if (tname.size() > MAX_SAFE_TERM_LENGTH)
		    throw Xapian::InvalidArgumentError("Term too long (> "STRINGIZE(MAX_SAFE_TERM_LENGTH)"): " + tname);
		map<string, pair<termcount_diff, termcount_diff> >::iterator i;
		i = freq_deltas.find(tname);
		if (i == freq_deltas.end()) {
		    freq_deltas.insert(make_pair(tname, make_pair(1, termcount_diff(wdf))));
		} else {
		    ++i->second.first;
		    i->second.second += wdf;
		}

		// Add did to tname's postlist
		map<string, map<docid, pair<char, termcount> > >::iterator j;
		j = mod_plists.find(tname);
		if (j == mod_plists.end()) {
		    map<docid, pair<char, termcount> > m;
		    j = mod_plists.insert(make_pair(tname, m)).first;
		}
		Assert(j->second.find(did) == j->second.end());
		j->second.insert(make_pair(did, make_pair('A', wdf)));

		if (term.positionlist_begin() != term.positionlist_end()) {
		    position_table.set_positionlist(
			did, tname,
			term.positionlist_begin(), term.positionlist_end());
		}
	    }
	}
	LOGLINE(DB, "Calculated doclen for new document " << did << " as " << new_doclen);

	// Set the termlist.
	if (termlist_table.is_open())
	    termlist_table.set_termlist(did, document, new_doclen);

	// Set the new document length
	Assert(doclens.find(did) == doclens.end() || doclens[did] == static_cast<Xapian::termcount>(-1));
	doclens[did] = new_doclen;
	stats.add_document(new_doclen);
    } catch (...) {
	// If an error occurs while adding a document, or doing any other
	// transaction, the modifications so far must be cleared before
	// returning control to the user - otherwise partial modifications will
	// persist in memory, and eventually get written to disk.
	cancel();
	throw;
    }

    // FIXME: this should be done by checking memory usage, not the number of
    // changes.
    // We could also look at:
    // * mod_plists.size()
    // * doclens.size()
    // * freq_deltas.size()
    //
    // cout << "+++ mod_plists.size() " << mod_plists.size() <<
    //     ", doclens.size() " << doclens.size() <<
    //	   ", freq_deltas.size() " << freq_deltas.size() << endl;
    if (++change_count >= flush_threshold) {
	flush_postlist_changes();
	if (!transaction_active()) apply();
    }

    RETURN(did);
}

void
ChertWritableDatabase::delete_document(Xapian::docid did)
{
    DEBUGCALL(DB, void, "ChertWritableDatabase::delete_document", did);
    Assert(did != 0);

    if (!termlist_table.is_open())
	throw Xapian::FeatureUnavailableError("Database has no termlist");

    if (rare(modify_shortcut_docid == did)) {
	// The modify_shortcut document can't be used for a modification
	// shortcut now, because it's been deleted!
	modify_shortcut_document = NULL;
	modify_shortcut_docid = 0;
    }

    // Remove the record.  If this fails, just propagate the exception since
    // the state should still be consistent (most likely it's
    // DocNotFoundError).
    record_table.delete_record(did);

    try {
	// Remove the values.
	value_manager.delete_document(did, value_stats);

	// OK, now add entries to remove the postings in the underlying record.
	Xapian::Internal::RefCntPtr<const ChertWritableDatabase> ptrtothis(this);
	ChertTermList termlist(ptrtothis, did);

	stats.delete_document(termlist.get_doclength());

	termlist.next();
	while (!termlist.at_end()) {
	    string tname = termlist.get_termname();
	    position_table.delete_positionlist(did, tname);
	    termcount wdf = termlist.get_wdf();

	    map<string, pair<termcount_diff, termcount_diff> >::iterator i;
	    i = freq_deltas.find(tname);
	    if (i == freq_deltas.end()) {
		freq_deltas.insert(make_pair(tname, make_pair(-1, -termcount_diff(wdf))));
	    } else {
		--i->second.first;
		i->second.second -= wdf;
	    }

	    // Remove did from tname's postlist
	    map<string, map<docid, pair<char, termcount> > >::iterator j;
	    j = mod_plists.find(tname);
	    if (j == mod_plists.end()) {
		map<docid, pair<char, termcount> > m;
		j = mod_plists.insert(make_pair(tname, m)).first;
	    }

	    map<docid, pair<char, termcount> >::iterator k;
	    k = j->second.find(did);
	    if (k == j->second.end()) {
		j->second.insert(make_pair(did, make_pair('D', 0u)));
	    } else {
		// Deleting a document we added/modified since the last flush.
		k->second = make_pair('D', 0u);
	    }

	    termlist.next();
	}

	// Remove the termlist.
	if (termlist_table.is_open())
	    termlist_table.delete_termlist(did);

	// Mark this document as removed.
	doclens[did] = static_cast<Xapian::termcount>(-1);
    } catch (...) {
	// If an error occurs while deleting a document, or doing any other
	// transaction, the modifications so far must be cleared before
	// returning control to the user - otherwise partial modifications will
	// persist in memory, and eventually get written to disk.
	cancel();
	throw;
    }

    if (++change_count >= flush_threshold) {
	flush_postlist_changes();
	if (!transaction_active()) apply();
    }
}

void
ChertWritableDatabase::replace_document(Xapian::docid did,
					const Xapian::Document & document)
{
    DEBUGCALL(DB, void, "ChertWritableDatabase::replace_document", did << ", " << document);
    Assert(did != 0);

    try {
	if (did > stats.get_last_docid()) {
	    stats.set_last_docid(did);
	    // If this docid is above the highwatermark, then we can't be
	    // replacing an existing document.
	    (void)add_document_(did, document);
	    return;
	}

	if (!termlist_table.is_open()) {
	    // We can replace an *unused* docid <= last_docid too.
	    Xapian::Internal::RefCntPtr<const ChertDatabase> ptrtothis(this);
	    if (!postlist_table.document_exists(did, ptrtothis)) {
		(void)add_document_(did, document);
		return;
	    }
	    throw Xapian::FeatureUnavailableError("Database has no termlist");
	}

	// Check for a document read from this database being replaced - ie, a
	// modification operation.
	bool modifying = false;
	if (modify_shortcut_docid &&
	    document.internal->get_docid() == modify_shortcut_docid) {
	    if (document.internal.get() == modify_shortcut_document) {
		// We have a docid, it matches, and the pointer matches, so we
		// can skip modification of any data which hasn't been modified
		// in the document.
		modifying = true;
		LOGLINE(DB, "Detected potential document modification shortcut.");
	    } else {
		// The modify_shortcut document can't be used for a
		// modification shortcut now, because it's about to be
		// modified.
		modify_shortcut_document = NULL;
		modify_shortcut_docid = 0;
	    }
	}

	if (!modifying || document.internal->terms_modified()) {
	    // FIXME - in the case where there is overlap between the new
	    // termlist and the old termlist, it would be better to compare the
	    // two lists, and make the minimum set of modifications required.
	    // This would lead to smaller changesets for replication, and
	    // probably be faster overall.

	    // First, add entries to remove the postings in the underlying
	    // record.
	    Xapian::Internal::RefCntPtr<const ChertWritableDatabase> ptrtothis(this);
	    ChertTermList termlist(ptrtothis, did);

	    termlist.next();
	    while (!termlist.at_end()) {
		string tname = termlist.get_termname();
		termcount wdf = termlist.get_wdf();

		map<string, pair<termcount_diff, termcount_diff> >::iterator i;
		i = freq_deltas.find(tname);
		if (i == freq_deltas.end()) {
		    freq_deltas.insert(make_pair(tname, make_pair(-1, -termcount_diff(wdf))));
		} else {
		    --i->second.first;
		    i->second.second -= wdf;
		}

		// Remove did from tname's postlist
		map<string, map<docid, pair<char, termcount> > >::iterator j;
		j = mod_plists.find(tname);
		if (j == mod_plists.end()) {
		    map<docid, pair<char, termcount> > m;
		    j = mod_plists.insert(make_pair(tname, m)).first;
		}

		map<docid, pair<char, termcount> >::iterator k;
		k = j->second.find(did);
		if (k == j->second.end()) {
		    j->second.insert(make_pair(did, make_pair('D', 0u)));
		} else {
		    // Modifying a document we added/modified since the last
		    // flush.
		    k->second = make_pair('D', 0u);
		}

		termlist.next();
	    }

	    stats.delete_document(termlist.get_doclength());

	    chert_doclen_t new_doclen = 0;
	    Xapian::TermIterator term = document.termlist_begin();
	    Xapian::TermIterator term_end = document.termlist_end();
	    for ( ; term != term_end; ++term) {
		termcount wdf = term.get_wdf();
		// Calculate the new document length
		new_doclen += wdf;
		stats.check_wdf(wdf);

		string tname = *term;
		if (tname.size() > MAX_SAFE_TERM_LENGTH)
		    throw Xapian::InvalidArgumentError("Term too long (> "STRINGIZE(MAX_SAFE_TERM_LENGTH)"): " + tname);
		map<string, pair<termcount_diff, termcount_diff> >::iterator i;
		i = freq_deltas.find(tname);
		if (i == freq_deltas.end()) {
		    freq_deltas.insert(make_pair(tname, make_pair(1, termcount_diff(wdf))));
		} else {
		    ++i->second.first;
		    i->second.second += wdf;
		}

		// Add did to tname's postlist
		map<string, map<docid, pair<char, termcount> > >::iterator j;
		j = mod_plists.find(tname);
		if (j == mod_plists.end()) {
		    map<docid, pair<char, termcount> > m;
		    j = mod_plists.insert(make_pair(tname, m)).first;
		}
		map<docid, pair<char, termcount> >::iterator k;
		k = j->second.find(did);
		if (k != j->second.end()) {
		    Assert(k->second.first == 'D');
		    k->second.first = 'M';
		    k->second.second = wdf;
		} else {
		    j->second.insert(make_pair(did, make_pair('A', wdf)));
		}

		PositionIterator it = term.positionlist_begin();
		PositionIterator it_end = term.positionlist_end();
		if (it != it_end) {
		    position_table.set_positionlist(did, tname, it, it_end);
		} else {
		    position_table.delete_positionlist(did, tname);
		}
	    }
	    LOGLINE(DB, "Calculated doclen for replacement document " << did << " as " << new_doclen);

	    // Set the termlist.
	    if (termlist_table.is_open())
		termlist_table.set_termlist(did, document, new_doclen);

	    // Set the new document length
	    doclens[did] = new_doclen;
	    stats.add_document(new_doclen);
	}

	if (!modifying || document.internal->data_modified()) {
	    // Replace the record
	    record_table.replace_record(document.get_data(), did);
	}

	if (!modifying || document.internal->values_modified()) {
	    // Replace the values.
	    value_manager.replace_document(did, document, value_stats);
	}
    } catch (const Xapian::DocNotFoundError &) {
	(void)add_document_(did, document);
	return;
    } catch (...) {
	// If an error occurs while replacing a document, or doing any other
	// transaction, the modifications so far must be cleared before
	// returning control to the user - otherwise partial modifications will
	// persist in memory, and eventually get written to disk.
	cancel();
	throw;
    }

    if (++change_count >= flush_threshold) {
	flush_postlist_changes();
	if (!transaction_active()) apply();
    }
}

Xapian::Document::Internal *
ChertWritableDatabase::open_document(Xapian::docid did, bool lazy) const
{
    DEBUGCALL(DB, Xapian::Document::Internal *, "ChertWritableDatabase::open_document",
	      did << ", " << lazy);
    modify_shortcut_document = ChertDatabase::open_document(did, lazy);
    // Store the docid only after open_document() successfully returns, so an
    // attempt to open a missing document doesn't overwrite this.
    modify_shortcut_docid = did;
    RETURN(modify_shortcut_document);
}

Xapian::termcount
ChertWritableDatabase::get_doclength(Xapian::docid did) const
{
    DEBUGCALL(DB, Xapian::termcount, "ChertWritableDatabase::get_doclength", did);
    map<docid, termcount>::const_iterator i = doclens.find(did);
    if (i != doclens.end()) {
	Xapian::termcount doclen = i->second;
	if (doclen == static_cast<Xapian::termcount>(-1)) {
	    throw Xapian::DocNotFoundError("Document " + om_tostring(did) + " not found");
	}
	RETURN(doclen);
    }
    RETURN(ChertDatabase::get_doclength(did));
}

Xapian::doccount
ChertWritableDatabase::get_termfreq(const string & tname) const
{
    DEBUGCALL(DB, Xapian::doccount, "ChertWritableDatabase::get_termfreq", tname);
    Xapian::doccount termfreq = ChertDatabase::get_termfreq(tname);
    map<string, pair<termcount_diff, termcount_diff> >::const_iterator i;
    i = freq_deltas.find(tname);
    if (i != freq_deltas.end()) termfreq += i->second.first;
    RETURN(termfreq);
}

Xapian::termcount
ChertWritableDatabase::get_collection_freq(const string & tname) const
{
    DEBUGCALL(DB, Xapian::termcount, "ChertWritableDatabase::get_collection_freq", tname);
    Xapian::termcount collfreq = ChertDatabase::get_collection_freq(tname);

    map<string, pair<termcount_diff, termcount_diff> >::const_iterator i;
    i = freq_deltas.find(tname);
    if (i != freq_deltas.end()) collfreq += i->second.second;

    RETURN(collfreq);
}

Xapian::doccount
ChertWritableDatabase::get_value_freq(Xapian::valueno valno) const
{
    DEBUGCALL(DB, Xapian::doccount, "ChertWritableDatabase::get_value_freq", valno);
    map<Xapian::valueno, ValueStats>::const_iterator i;
    i = value_stats.find(valno);
    if (i != value_stats.end()) RETURN(i->second.freq);
    RETURN(ChertDatabase::get_value_freq(valno));
}

std::string
ChertWritableDatabase::get_value_lower_bound(Xapian::valueno valno) const
{
    DEBUGCALL(DB, std::string, "ChertWritableDatabase::get_value_lower_bound", valno);
    map<Xapian::valueno, ValueStats>::const_iterator i;
    i = value_stats.find(valno);
    if (i != value_stats.end()) RETURN(i->second.lower_bound);
    RETURN(ChertDatabase::get_value_lower_bound(valno));
}

std::string
ChertWritableDatabase::get_value_upper_bound(Xapian::valueno valno) const
{
    DEBUGCALL(DB, std::string, "ChertWritableDatabase::get_value_upper_bound", valno);
    map<Xapian::valueno, ValueStats>::const_iterator i;
    i = value_stats.find(valno);
    if (i != value_stats.end()) RETURN(i->second.upper_bound);
    RETURN(ChertDatabase::get_value_upper_bound(valno));
}

bool
ChertWritableDatabase::term_exists(const string & tname) const
{
    DEBUGCALL(DB, bool, "ChertWritableDatabase::term_exists", tname);
    RETURN(get_termfreq(tname) != 0);
}

LeafPostList *
ChertWritableDatabase::open_post_list(const string& tname) const
{
    DEBUGCALL(DB, LeafPostList *, "ChertWritableDatabase::open_post_list", tname);
    Xapian::Internal::RefCntPtr<const ChertWritableDatabase> ptrtothis(this);

    if (tname.empty()) {
	Xapian::doccount doccount = get_doccount();
	if (stats.get_last_docid() == doccount) {
	    RETURN(new ContiguousAllDocsPostList(ptrtothis, doccount));
	}
	if (doclens.empty()) {
	    RETURN(new ChertAllDocsPostList(ptrtothis, doccount));
	}
	RETURN(new ChertAllDocsModifiedPostList(ptrtothis, doccount, doclens));
    }

    map<string, map<docid, pair<char, termcount> > >::const_iterator j;
    j = mod_plists.find(tname);
    if (j != mod_plists.end()) {
	// We've got buffered changes to this term's postlist, so we need to
	// use a ChertModifiedPostList.
	RETURN(new ChertModifiedPostList(ptrtothis, tname, j->second));
    }

    RETURN(new ChertPostList(ptrtothis, tname, true));
}

ValueList *
ChertWritableDatabase::open_value_list(Xapian::valueno slot) const
{
    DEBUGCALL(DB, ValueList *, "ChertWritableDatabase::open_value_list", slot);
    // If there are changes, we don't have code to iterate the modified value
    // list so we need to flush (but don't commit - there may be a transaction
    // in progress).
    if (change_count) value_manager.merge_changes();
    RETURN(ChertDatabase::open_value_list(slot));
}

TermList *
ChertWritableDatabase::open_allterms(const string & prefix) const
{
    DEBUGCALL(DB, TermList *, "ChertWritableDatabase::open_allterms", "");
    // If there are changes, terms may have been added or removed, and so we
    // need to flush (but don't commit - there may be a transaction in
    // progress).
    if (change_count) flush_postlist_changes();
    RETURN(ChertDatabase::open_allterms(prefix));
}

void
ChertWritableDatabase::cancel()
{
    ChertDatabase::cancel();
    stats.read(postlist_table);
    freq_deltas.clear();
    doclens.clear();
    mod_plists.clear();
    value_stats.clear();
    change_count = 0;
}

void
ChertWritableDatabase::add_spelling(const string & word,
				    Xapian::termcount freqinc) const
{
    spelling_table.add_word(word, freqinc);
}

void
ChertWritableDatabase::remove_spelling(const string & word,
				       Xapian::termcount freqdec) const
{
    spelling_table.remove_word(word, freqdec);
}

TermList *
ChertWritableDatabase::open_spelling_wordlist() const
{
    spelling_table.merge_changes();
    return ChertDatabase::open_spelling_wordlist();
}

TermList *
ChertWritableDatabase::open_synonym_keylist(const string & prefix) const
{
    synonym_table.merge_changes();
    return ChertDatabase::open_synonym_keylist(prefix);
}

void
ChertWritableDatabase::add_synonym(const string & term,
				   const string & synonym) const
{
    synonym_table.add_synonym(term, synonym);
}

void
ChertWritableDatabase::remove_synonym(const string & term,
				      const string & synonym) const
{
    synonym_table.remove_synonym(term, synonym);
}

void
ChertWritableDatabase::clear_synonyms(const string & term) const
{
    synonym_table.clear_synonyms(term);
}

void
ChertWritableDatabase::set_metadata(const string & key, const string & value)
{
    DEBUGCALL(DB, string, "ChertWritableDatabase::set_metadata",
	      key << ", " << value);
    string btree_key("\x00\xc0", 2);
    btree_key += key;
    if (value.empty()) {
	postlist_table.del(btree_key);
    } else {
	postlist_table.add(btree_key, value);
    }
}

void
ChertWritableDatabase::invalidate_doc_object(Xapian::Document::Internal * obj) const
{
    if (obj == modify_shortcut_document) {
	modify_shortcut_document = NULL;
	modify_shortcut_docid = 0;
    }
}
