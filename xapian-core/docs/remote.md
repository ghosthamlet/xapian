% Remote Backend

This document describes how to make use of the facilities in Xapian
for distributed searches.

## Overview

There are two sides to the distributed searching.  The client end is the
program initiating the search on behalf of a user, and the server end is
the program which provides a searching interface over a set of databases
for the client.  There can be many servers, with many clients sharing
them.  In theory, a server can also be a client to other servers, but
this may not be very useful or efficient.

The client runs queries in the same way that it would on local databases
- the difference is in how the database is opened.
Once the database is opened, the query process is identical to any other.
Using a stub database with "auto" backend is a good way to wrap up access to
a remote database in a neat way.

The remote backend currently support two client/server methods: prog and
tcp.  They both use the same protocol, although different means to contact
the server.

## The Prog Method

The prog method spawns a program when the database is opened, and
communicates with it over a pipe.  This can be used to connect to a
remote Xapian database across an SSH tunnel for example, providing
authentication and encryption.  The xapian-progsrv program is designed
to be the program at the far end of the connection.

From the client end, create the database with <code>Xapian::Database database(Xapian::Remote::open(<i>program</i>, <i>args</i>));</code> - for example:

    Xapian::Database database(Xapian::Remote::open("ssh",
            "search.example.com xapian-progsrv /var/lib/xapian/data/db1"));

If the program has no path, the PATH environment variable is used.

## The TCP Method

The tcp method uses TCP/IP sockets to connect to a running server on a remote
machine (or indeed a local one, but that's rather pointless!)

From the client end, create the database with <code>Xapian::Database database(Xapian::Remote::open(<i>host</i>, <i>port</i>));</code> - for example:

    Xapian::Database database(Xapian::Remote::open("searchserver", 33333));

The host is the server's hostname, the port is the tcp port on the server to
use.

The server is xapian-tcpsrv, which is installed by xapian-core's
"<code>make&nbsp;install</code>".
This should be started and left running in the background before searches
are performed.

The arguments xapian-tcpsrv currently knows are:

<dl>
<dt> --port PORTNUM
<dd>        (required) the port to listen on.
<dt>    --one-shot
<dd>        Handle one connection, and then exit.  If --one-shot is not used,
	then the server runs until it is killed manually.
<dt>    --idle-timeout MSECS
<dd>	Set the timeout on a idle connection.
<dt>    --active-timeout MSECS
<dd>	Set the timeout waiting for responses when the connection is active.
<dt>    --timeout MSECS
<dd>	Set the idle and active timeouts to the same value.
<dt>    --quiet
<dd>	Minimal output.
</dl>

One or more databases need to be specified by listing their directories - they
are opened using the "auto" pseudo-backend.

Once started, the server will run and listen for connections on the
configured port.  Each connection is handled by a forked child process
(or a new thread under Windows), so concurrent read access is supported.

## Notes

A remote search should behave just like the equivalent local one, except
a few features aren't currently implemented (e.g. spelling, synonyms, user
metadata).

Exceptions are propagated across the link and thrown again at the client end.

The remote backend now support writable databases.  Just start
<code>xapian-progsrv</code> or <code>xapian-tcpsrv</code> with the
option <code>--writable</code>.  Only one database may be specified.