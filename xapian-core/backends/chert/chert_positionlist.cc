/* chert_positionlist.cc: A position list in a chert database.
 *
 * Copyright (C) 2004,2005,2006,2008 Olly Betts
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

#include "chert_positionlist.h"

#include <xapian/types.h>

#include "bitstream.h"
#include "omdebug.h"
#include "pack.h"

#include <string>
#include <vector>

using namespace std;

void
ChertPositionListTable::set_positionlist(Xapian::docid did,
					 const string & tname,
					 Xapian::PositionIterator pos,
					 const Xapian::PositionIterator &pos_end)
{
    DEBUGCALL(DB, void, "ChertPositionList::set_positionlist",
	      did << ", " << tname << ", " << pos << ", " << pos_end);
    Assert(pos != pos_end);

    // FIXME: avoid the need for this copy!
    vector<Xapian::termpos> poscopy(pos, pos_end);

    string key = make_key(did, tname);

    string s;
    pack_uint(s, poscopy.back());

    if (poscopy.size() == 1) {
	// Special case for single entry position list.
	add(key, s);
	return;
    }

    BitWriter wr(s);
    wr.encode(poscopy[0], poscopy.back());
    wr.encode(poscopy.size() - 2, poscopy.back() - poscopy[0]);
    wr.encode_interpolative(poscopy, 0, poscopy.size() - 1);
    add(key, wr.freeze());
}

Xapian::termcount
ChertPositionListTable::positionlist_count(Xapian::docid did,
					   const string & term) const
{
    DEBUGCALL(DB, void, "ChertPositionListTable::positionlist_count",
	      did << ", " << term);

    string data;
    if (!get_exact_entry(make_key(did, term), data)) {
	// There's no positional information for this term.
	return 0;
    }

    const char * pos = data.data();
    const char * end = pos + data.size();
    Xapian::termpos pos_last;
    if (!unpack_uint(&pos, end, &pos_last)) {
	throw Xapian::DatabaseCorruptError("Position list data corrupt");
    }
    if (pos == end) {
	// Special case for single entry position list.
	return 1;
    }

    // Skip the header we just read.
    BitReader rd(data, pos - data.data());
    Xapian::termpos pos_first = rd.decode(pos_last);
    Xapian::termpos pos_size = rd.decode(pos_last - pos_first) + 2;
    return pos_size;
}

///////////////////////////////////////////////////////////////////////////

bool
ChertPositionList::read_data(const ChertTable * table, Xapian::docid did,
			     const string & tname)
{
    DEBUGCALL(DB, void, "ChertPositionList::read_data",
	      table << ", " << did << ", " << tname);

    have_started = false;
    positions.clear();

    string data;
    if (!table->get_exact_entry(ChertPositionListTable::make_key(did, tname), data)) {
	// There's no positional information for this term.
	current_pos = positions.begin();
	return false;
    }

    const char * pos = data.data();
    const char * end = pos + data.size();
    Xapian::termpos pos_last;
    if (!unpack_uint(&pos, end, &pos_last)) {
	throw Xapian::DatabaseCorruptError("Position list data corrupt");
    }
    if (pos == end) {
	// Special case for single entry position list.
	positions.push_back(pos_last);
	current_pos = positions.begin();
	return true;
    }
    // Skip the header we just read.
    BitReader rd(data, pos - data.data());
    Xapian::termpos pos_first = rd.decode(pos_last);
    Xapian::termpos pos_size = rd.decode(pos_last - pos_first) + 2;
    positions.resize(pos_size);
    positions[0] = pos_first;
    positions.back() = pos_last;
    rd.decode_interpolative(positions, 0, pos_size - 1);

    current_pos = positions.begin();
    return true;
}

Xapian::termcount
ChertPositionList::get_size() const
{
    DEBUGCALL(DB, Xapian::termcount, "ChertPositionList::get_size", "");
    RETURN(positions.size());
}

Xapian::termpos
ChertPositionList::get_position() const
{
    DEBUGCALL(DB, Xapian::termpos, "ChertPositionList::get_position", "");
    Assert(have_started);
    RETURN(*current_pos);
}

void
ChertPositionList::next()
{
    DEBUGCALL(DB, void, "ChertPositionList::next", "");

    if (!have_started) {
	have_started = true;
    } else {
	Assert(!at_end());
	++current_pos;
    }
}

void
ChertPositionList::skip_to(Xapian::termpos termpos)
{
    DEBUGCALL(DB, void, "ChertPositionList::skip_to", termpos);
    if (!have_started) {
	have_started = true;
    }
    while (!at_end() && *current_pos < termpos) ++current_pos;
}

bool
ChertPositionList::at_end() const
{
    DEBUGCALL(DB, bool, "ChertPositionList::at_end", "");
    RETURN(current_pos == positions.end());
}
