//=============================================================================
// Brief   : DNS Resolver
// Authors : Carlos Guimaraes <cguimaraes@av.it.pt>
//------------------------------------------------------------------------------
// ODTONE - Open Dot Twenty One
//
// Copyright (C) 2009-2011 Universidade Aveiro
// Copyright (C) 2009-2011 Instituto de Telecomunicações - Pólo Aveiro
//
// This software is distributed under a license. The full license
// agreement can be found in the file LICENSE in this distribution.
// This software may not be copied, modified, sold or distributed
// other than expressed in the named license agreement.
//
// This software is distributed without any warranty.
//==============================================================================

#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <iostream>

#ifdef _WIN32
	#pragma comment(lib,"ws2_32")
	#pragma comment(lib,"advapi32")
	#include <winsock.h>
	typedef	int		socklen_t;
	typedef	unsigned char	uint8_t;
	typedef	unsigned short	uint16_t;
	typedef	unsigned int	uint32_t;
#else
	#define	closesocket(x)	close(x)
	#include <sys/types.h>
	#include <sys/socket.h>
	#include <sys/select.h>
	#include <netinet/in.h>
	#include <unistd.h>
#endif /* _WIN32 */

#include <odtone/net/dns/resolver.hpp>
#include <odtone/net/dns/message.hpp>
#include <odtone/net/dns/frame.hpp>
#include <odtone/net/dns/utils.hpp>

#include <boost/thread.hpp>
#include <boost/foreach.hpp>
#include <boost/optional.hpp>
#include <boost/algorithm/string.hpp>

///////////////////////////////////////////////////////////////////////////////
namespace odtone { namespace dns {

/**
 * Put the given file descriptor in non-blocking mode.
 *
 * @param fd The file descriptor to put in non-blocking mode.
 * @return 0 if success or -1 otherwise.
 */
static int nonblock(int fd)
{
#ifdef	_WIN32
	unsigned long	on = 1;
	return (ioctlsocket(fd, FIONBIO, &on));
#else
	int	flags;

	flags = fcntl(fd, F_GETFL, 0);

	return (fcntl(fd, F_SETFL, flags | O_NONBLOCK));
#endif /* _WIN32 */
}

/**
 * Find what DNS server to use.
 *
 * @param dns The resolver descriptor.
 * @return 0 if OK, -1 if error.
 */
static int getdnsip(struct dns &dns)
{
	int	ret = 0;

#ifdef _WIN32
	int	i;
	LONG	err;
	HKEY	hKey, hSub;
	char	subkey[512], dhcpns[512], ns[512], value[128], *key =
	"SYSTEM\\ControlSet001\\Services\\Tcpip\\Parameters\\Interfaces";

	if ((err = RegOpenKey(HKEY_LOCAL_MACHINE,
	    key, &hKey)) != ERROR_SUCCESS) {
		fprintf(stderr, "cannot open reg key %s: %d\n", key, err);
		ret--;
	} else {
		for (ret--, i = 0; RegEnumKey(hKey, i, subkey,
		    sizeof(subkey)) == ERROR_SUCCESS; i++) {
			DWORD type, len = sizeof(value);
			if (RegOpenKey(hKey, subkey, &hSub) == ERROR_SUCCESS &&
			    (RegQueryValueEx(hSub, "NameServer", 0,
			    &type, value, &len) == ERROR_SUCCESS ||
			    RegQueryValueEx(hSub, "DhcpNameServer", 0,
			    &type, value, &len) == ERROR_SUCCESS)) {
				dns.sa.sin_addr.s_addr = inet_addr(value);
				ret++;
				RegCloseKey(hSub);
				break;
			}
		}
		RegCloseKey(hKey);
	}
#else
	FILE	*fp;
	char	line[512];
	int	a, b, c, d;

	if ((fp = fopen("/etc/resolv.conf", "r")) == NULL) {
		ret--;
	} else {
		for (ret--; fgets(line, sizeof(line), fp) != NULL; ) {
			if (sscanf(line, "nameserver %d.%d.%d.%d",
			   &a, &b, &c, &d) == 4) {
				dns.sa.sin_addr.s_addr =
				    htonl(a << 24 | b << 16 | c << 8 | d);
				ret++;
				break;
			}
		}
		(void) fclose(fp);
	}
#endif /* _WIN32 */

	return (ret);
}

/**
 * Initializer the resolver descriptor.
 */
void resolver::dns_init()
{
	int		rcvbufsiz = 128 * 1024;

#ifdef _WIN32
	{ WSADATA data; WSAStartup(MAKEWORD(2,2), &data); }
#endif /* _WIN32 */

	if ((dns.sock = socket(PF_INET, SOCK_DGRAM, 17)) == -1)
		return;
	else if (nonblock(dns.sock) != 0)
		return;

	dns.sa.sin_family = AF_INET;
	dns.sa.sin_port = htons(53);

	/* Increase socket's receive buffer */
	(void) setsockopt(dns.sock, SOL_SOCKET, SO_RCVBUF,
	    (char *) &rcvbufsiz, sizeof(rcvbufsiz));
}

/*
 * Find host in host cache. Add it if not found.
 */
static boost::optional<struct query> find_cached_query(struct dns &dns, enum dns_query_type qtype, std::string name)
{
	boost::optional<struct query> query;

	BOOST_FOREACH(struct query tmp, dns.cached) {
		if(tmp.qtype == qtype && name.compare(tmp.name) == 0) {
			dns.cached.push_back(tmp);
			query = tmp;
			break;
		}
	}

	return query;
}

static boost::optional<struct query> find_active_query(struct dns &dns, uint16_t tid)
{
	boost::optional<struct query> query;

	BOOST_FOREACH(struct query tmp, dns.active) {
		if (tid == tmp.tid) {
			query = tmp;
			break;
		}
	}

	return query;
}

static void call_user(struct dns &dns, struct query query, enum dns_error error)
{
	struct dns_cb_data cbd;

	cbd.context	= query.ctx;
	cbd.query_type	= (enum dns_query_type) query.qtype;
	cbd.error	= error;
	cbd.name	= query.name;
	(void) memcpy(&cbd.pkt, query.pkt, query.pkt_len);
	cbd.pkt_len	= query.pkt_len;

	query.callback(&cbd);

	/* Move query to cache */
	dns.cached.push_back(query);
}

static void parse_udp(struct dns &dns, const unsigned char *pkt, int len)
{
	struct header		*header;
	const unsigned char	*p, *e;
	boost::optional<struct query> q;
	uint16_t			type;
	int					stop, dlen, nlen;

	/* We sent 1 query. We want to see more that 1 answer. */
	header = (struct header *) pkt;
	if (ntohs(header->nqueries) != 1)
		return;

	/* Return if we did not send that query */
	q = find_active_query(dns, header->tid);
	if (!q.is_initialized())
		return;

	// Associate the packet with the query
	q->pkt_len = len;
	(void) memcpy(&q->pkt, pkt, len);

	/* Received 0 answers */
	if (header->nanswers == 0) {
		call_user(dns, q.get(), DNS_DOES_NOT_EXIST);
		return;
	}

	/* Skip host name */
	for (e = pkt + len, nlen = 0, p = &header->data[0];
	    p < e && *p != '\0'; p++)
		nlen++;

#define	NTOHS(p)	(((p)[0] << 8) | (p)[1])

	/* We sent query class 1, query type 1 */
	if (&p[5] > e || NTOHS(p + 1) != q->qtype)
		return;

	/* Go to the first answer section */
	p += 5;

	/* Loop through the answers, we want A type answer */
	for (stop = 0; !stop && &p[12] < e; ) {

		/* Skip possible name in CNAME answer */
		if (*p != 0xc0) {
			while (*p && &p[12] < e)
				p++;
			p--;
		}

		type = htons(((uint16_t *)p)[1]);

		if (type == 5) {
			/* CNAME answer. shift to the next section */
			dlen = htons(((uint16_t *) p)[5]);
			p += 12 + dlen;
		} else if (type == q->qtype) {
			stop = 1;
			call_user(dns, q.get(), DNS_OK);
		} else {
			stop = 1;
		}
	}
}

/**
 * Check if there are pending messages.
 */
int resolver::dns_poll()
{
	struct sockaddr_in	sa;
	socklen_t			len = sizeof(sa);
	int					n, num_packets = 0;
	unsigned char		pkt[DNS_PACKET_LEN];
	time_t				now;

	now = time(NULL);

	/* Check our socket for new stuff */
	while ((n = recvfrom(dns.sock, pkt, sizeof(pkt), 0,
	    (struct sockaddr *) &sa, &len)) > 0 &&
	    n > (int) sizeof(struct header)) {
		parse_udp(dns, pkt, n);
		num_packets++;
	}

	/* Cleanup expired active queries */
	BOOST_FOREACH(struct query tmp, dns.active) {
		if (tmp.expire < now) {
			tmp.pkt_len = 0;
			call_user(dns, tmp, DNS_TIMEOUT);
			dns.active.remove(tmp);
		}
	}

	/* Cleanup cached queries */
	BOOST_FOREACH(struct query tmp, dns.cached) {
		if (tmp.expire < now) {
			dns.cached.remove(tmp);
		}
	}

	return (num_packets);
}

/**
 * Cleanup the resolver descriptor.
 */
void resolver::dns_fini()
{
	if (dns.sock != -1)
		(void) closesocket(dns.sock);

	BOOST_FOREACH(struct query tmp, dns.active) {
		dns.active.remove(tmp);
	}

	BOOST_FOREACH(struct query tmp, dns.cached) {
		dns.cached.remove(tmp);
	}
}

/**
 * Queue the resolution.
 *
 * @param ctx Query context.
 * @param name Query to be performed.
 * @param qtype Query type.
 * @param callback Callback function.
 */
void resolver::dns_queue(void *ctx, std::string name,
		enum dns_query_type qtype, dns_callback_t callback)
{
	struct header		*header;
	int					n;
	char				pkt[DNS_PACKET_LEN], *p;
	time_t				now = time(NULL);
	struct dns_cb_data	cbd;

	/* Search the cache first */
	boost::optional<struct query> cached_query;
	cached_query = find_cached_query(dns, qtype, name);
	if (cached_query.is_initialized()) {
		cached_query->ctx = ctx;
		call_user(dns, cached_query.get(), DNS_OK);
		if (cached_query->expire < now) {
			dns.cached.remove(cached_query.get());
		}
		return;
	}

	/* DNS packet header */
	header				= (struct header *) pkt;
	header->tid			= ++(dns.tid);
	header->flags		= htons(0x100);
	header->nqueries	= htons(1);
	header->nanswers	= 0;
	header->nauth		= 0;
	header->nother		= 0;

	/* Encode DNS name */
	p = (char *) &header->data;	/* For encoding host name into packet */

	std::vector<std::string> split_domain;
	boost::split(split_domain, name, boost::is_any_of("."));

	BOOST_FOREACH(std::string tmp, split_domain) {
		*p++ = tmp.size();
		for(int i = 0; i < tmp.size(); ++i) {
			*p++ = tmp[i];
			*p += tmp.size();
		}
	}
	*p++ = 0;	// Domain name terminator

	// Query type
	*p++ = 0;
	*p++ = (unsigned char) qtype;

	// Query Class
	*p++ = 0;
	*p++ = 1;

	assert(p < pkt + sizeof(pkt));
	n = p - pkt;			/* Total packet length */

	if (sendto(dns.sock, pkt, n, 0,
	    (struct sockaddr *) &dns.sa, sizeof(dns.sa)) < 0) {
		(void) memset(&cbd, 0, sizeof(cbd));
		cbd.error = DNS_ERROR;
		callback(&cbd);
	}

	/* Init query structure */
	struct query query;
	query.ctx	= ctx;
	query.qtype	= (uint16_t) qtype;
	query.tid	= header->tid;
	query.callback	= callback;
	query.expire	= now + DNS_QUERY_TIMEOUT;
	boost::algorithm::to_lower(name);
	query.name = name;

	dns.active.push_back(query);
}

/**
 * Construct the DNS resolver.
 */
resolver::resolver()
{
	// Initialize the resolver descriptor
	dns_init();
}

/**
 * Destruct the DNS resolver.
 */
resolver::~resolver()
{
	dns_fini();
}

/**
 * Queries the DNS server.
 *
 * @param host Query to be performed.
 * @param type Query type.
 * @param callback Callback function.
 */
void resolver::queue(std::string host, enum dns_query_type type, dns_callback_t callback)
{
	fd_set set;
	struct timeval tv = {0, 100000};

	// Get the DNS Server IP address
	if (getdnsip(dns) != 0)
		return;

	dns_queue(&host, host, type, callback);

	FD_ZERO(&set);
	FD_SET(dns.sock, &set);

	if (select(dns.sock + 1, &set, NULL, NULL, &tv) == 1)
		dns_poll();
}

/**
 * Cancel the query.
 *
 * @param context The query context.
 */
void resolver::dns_cancel(const void *context)
{
	BOOST_FOREACH(struct query tmp, dns.active) {
		if (tmp.ctx == context) {
			dns.active.remove(tmp);
			break;
		}
	}
}


///////////////////////////////////////////////////////////////////////////////
} /* namespace dns */ } /* namespace odtone */

// EOF ////////////////////////////////////////////////////////////////////////
