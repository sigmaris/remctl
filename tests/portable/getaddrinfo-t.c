/*
 * getaddrinfo test suite.
 *
 * Copyright (c) 2004, 2005, 2006, 2007
 *     by Internet Systems Consortium, Inc. ("ISC")
 * Copyright (c) 1991, 1994, 1995, 1996, 1997, 1998, 1999, 2000, 2001,
 *     2002, 2003 by The Internet Software Consortium and Rich Salz
 *
 * This code is derived from software contributed to the Internet Software
 * Consortium by Rich Salz.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <config.h>
#include <portable/system.h>
#include <portable/socket.h>

#include <tests/libtest.h>
#include <util/util.h>

/*
 * If the native platform doesn't support AI_NUMERICSERV or AI_NUMERICHOST,
 * pick some other values for them that match the values set in our
 * implementation.
 */
#if AI_NUMERICSERV == 0
# undef AI_NUMERICSERV
# define AI_NUMERICSERV 0x0080
#endif
#if AI_NUMERICHOST == 0
# undef AI_NUMERICHOST
# define AI_NUMERICHOST 0x0100
#endif

const char *test_gai_strerror(int);
void test_freeaddrinfo(struct addrinfo *);
int test_getaddrinfo(const char *, const char *, const struct addrinfo *,
                     struct addrinfo **);

int
main(void)
{
    struct addrinfo *ai, *first;
    struct addrinfo hints;
    struct sockaddr_in *saddr;
    struct hostent *host;
    struct in_addr addr;
    struct servent *service;
    int i;
    int found;

    test_init(75);

    ok_string(1, "Host name lookup failure", test_gai_strerror(1));
    ok_string(2, "System error", test_gai_strerror(9));
    ok_string(3, "Unknown error", test_gai_strerror(40));
    ok_string(4, "Unknown error", test_gai_strerror(-37));

    ok(5, test_getaddrinfo(NULL, "25", NULL, &ai) == 0);
    ok(6, ai->ai_family == AF_INET);
    ok(7, ai->ai_socktype == 0);
    ok(8, ai->ai_protocol == IPPROTO_TCP);
    ok(9, ai->ai_canonname == NULL);
    ok(10, ai->ai_addrlen == sizeof(struct sockaddr_in));
    saddr = (struct sockaddr_in *) ai->ai_addr;
    ok(11, saddr->sin_port == htons(25));
    ok(12, saddr->sin_addr.s_addr == INADDR_LOOPBACK);
    test_freeaddrinfo(ai);

    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = AI_PASSIVE;
    hints.ai_socktype = SOCK_STREAM;
    ok(13, test_getaddrinfo(NULL, "25", &hints, &ai) == 0);
    ok(14, ai->ai_socktype == SOCK_STREAM);
    saddr = (struct sockaddr_in *) ai->ai_addr;
    ok(15, saddr->sin_port == htons(25));
    ok(16, saddr->sin_addr.s_addr == INADDR_ANY);
    test_freeaddrinfo(ai);

    service = getservbyname("smtp", "tcp");
    if (service == NULL)
        skip_block(17, 4, "smtp service not found");
    else {
        hints.ai_socktype = 0;
        ok(17, test_getaddrinfo(NULL, "smtp", &hints, &ai) == 0);
        ok(18, ai->ai_socktype == SOCK_STREAM);
        saddr = (struct sockaddr_in *) ai->ai_addr;
        ok(19, saddr->sin_port == htons(25));
        ok(20, saddr->sin_addr.s_addr == INADDR_ANY);
        test_freeaddrinfo(ai);
    }

    hints.ai_flags = AI_NUMERICSERV;
    ok(21, test_getaddrinfo(NULL, "smtp", &hints, &ai) == EAI_NONAME);
    ok(22, test_getaddrinfo(NULL, "25 smtp", &hints, &ai) == EAI_NONAME);
    ok(23, test_getaddrinfo(NULL, "25 ", &hints, &ai) == EAI_NONAME);
    ok(24, test_getaddrinfo(NULL, "25", &hints, &ai) == 0);
    saddr = (struct sockaddr_in *) ai->ai_addr;
    ok(25, saddr->sin_port == htons(25));
    ok(26, saddr->sin_addr.s_addr == INADDR_LOOPBACK);
    test_freeaddrinfo(ai);

    ok(27, test_getaddrinfo(NULL, NULL, NULL, &ai) == EAI_NONAME);
    hints.ai_flags = 2000;
    ok(28, test_getaddrinfo(NULL, "25", &hints, &ai) == EAI_BADFLAGS);
    hints.ai_flags = 0;
    hints.ai_socktype = SOCK_RAW;
    ok(29, test_getaddrinfo(NULL, "25", &hints, &ai) == EAI_SOCKTYPE);
    hints.ai_socktype = 0;
    hints.ai_family = AF_UNIX;
    ok(30, test_getaddrinfo(NULL, "25", &hints, &ai) == EAI_FAMILY);
    hints.ai_family = AF_UNSPEC;

    inet_aton("10.20.30.40", &addr);
    ok(31, test_getaddrinfo("10.20.30.40", NULL, NULL, &ai) == 0);
    ok(32, ai->ai_family == AF_INET);
    ok(33, ai->ai_socktype == 0);
    ok(34, ai->ai_protocol == IPPROTO_TCP);
    ok(35, ai->ai_canonname == NULL);
    ok(36, ai->ai_addrlen == sizeof(struct sockaddr_in));
    saddr = (struct sockaddr_in *) ai->ai_addr;
    ok(37, saddr->sin_port == 0);
    ok(38, saddr->sin_addr.s_addr == addr.s_addr);
    test_freeaddrinfo(ai);

    if (service == NULL)
        skip_block(39, 7, "smtp service not found");
    else {
        ok(39, test_getaddrinfo("10.20.30.40", "smtp", &hints, &ai) == 0);
        ok(40, ai->ai_family == AF_INET);
        ok(41, ai->ai_socktype == SOCK_STREAM);
        ok(42, ai->ai_protocol == IPPROTO_TCP);
        ok(43, ai->ai_canonname == NULL);
        saddr = (struct sockaddr_in *) ai->ai_addr;
        ok(44, saddr->sin_port == htons(25));
        ok(45, saddr->sin_addr.s_addr == addr.s_addr);
        test_freeaddrinfo(ai);
    }

    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    ok(46, test_getaddrinfo("10.2.3.4", "smtp", &hints, &ai) == EAI_NONAME);
    ok(47, test_getaddrinfo("example.com", "25", &hints, &ai) == EAI_NONAME);
    ok(48, test_getaddrinfo("10.20.30.40", "25", &hints, &ai) == 0);
    saddr = (struct sockaddr_in *) ai->ai_addr;
    ok(49, saddr->sin_port == htons(25));
    ok(50, saddr->sin_addr.s_addr == addr.s_addr);
    test_freeaddrinfo(ai);

    if (service == NULL)
        skip_block(51, 4, "smtp service not found");
    else {
        hints.ai_flags = AI_NUMERICHOST | AI_CANONNAME;
        ok(51, test_getaddrinfo("10.20.30.40", "smtp", &hints, &ai) == 0);
        ok_string(52, "10.20.30.40", ai->ai_canonname);
        saddr = (struct sockaddr_in *) ai->ai_addr;
        ok(53, saddr->sin_port == htons(25));
        ok(54, saddr->sin_addr.s_addr == addr.s_addr);
        test_freeaddrinfo(ai);
    }

    service = getservbyname("domain", "udp");
    if (service == NULL)
        skip_block(55, 5, "domain service not found");
    else {
        hints.ai_flags = 0;
        hints.ai_socktype = SOCK_DGRAM;
        ok(55, test_getaddrinfo("10.20.30.40", "domain", &hints, &ai) == 0);
        ok(56, ai->ai_socktype == SOCK_DGRAM);
        ok(57, ai->ai_canonname == NULL);
        saddr = (struct sockaddr_in *) ai->ai_addr;
        ok(58, saddr->sin_port == htons(53));
        ok(59, saddr->sin_addr.s_addr == addr.s_addr);
        test_freeaddrinfo(ai);
    }

    /* Hopefully this will always resolve. */
    host = gethostbyname("www.isc.org");
    if (host == NULL)
        skip_block(60, 9, "cannot look up www.isc.org");
    else {
        hints.ai_flags = 0;
        hints.ai_socktype = SOCK_STREAM;
        ok(60, test_getaddrinfo("www.isc.org", "80", &hints, &ai) == 0);
        ok(61, ai->ai_socktype == SOCK_STREAM);
        ok(62, ai->ai_canonname == NULL);
        saddr = (struct sockaddr_in *) ai->ai_addr;
        ok(63, saddr->sin_port == htons(80));
        ok(64, saddr->sin_addr.s_addr != INADDR_ANY);
        addr = saddr->sin_addr;
        test_freeaddrinfo(ai);

        hints.ai_flags = AI_CANONNAME;
        ok(65, test_getaddrinfo("www.isc.org", "80", &hints, &ai) == 0);
        ok(66, ai->ai_canonname != NULL);
        saddr = (struct sockaddr_in *) ai->ai_addr;
        ok(67, saddr->sin_port == htons(80));
        ok(68, saddr->sin_addr.s_addr == addr.s_addr);
        test_freeaddrinfo(ai);
    }

    /* Included because it had multiple A records. */
    host = gethostbyname("cnn.com");
    if (host == NULL)
        skip_block(69, 3, "cannot look up cnn.com");
    else {
        ok(69, test_getaddrinfo("cnn.com", "80", NULL, &ai) == 0);
        saddr = (struct sockaddr_in *) ai->ai_addr;
        ok(70, saddr->sin_port == htons(80));
        ok(71, saddr->sin_addr.s_addr != INADDR_ANY);
        addr = saddr->sin_addr;
        test_freeaddrinfo(ai);
    }

    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_CANONNAME;
    ok(72, test_getaddrinfo("foo.invalid", NULL, NULL, &ai) == EAI_NONAME);
    host = gethostbyname("cnn.com");
    if (host == NULL) {
        skip_block(73, 3, "cannot look up cnn.com");
        exit(0);
    }
    ok(73, test_getaddrinfo("cnn.com", NULL, &hints, &ai) == 0);
    saddr = (struct sockaddr_in *) ai->ai_addr;
    ok(74, saddr->sin_port == 0);
    first = ai;
    for (found = 0; ai != NULL; ai = ai->ai_next) {
        if (!strcmp(ai->ai_canonname, first->ai_canonname) == 0) {
            ok(75, 0);
            break;
        }
        if (ai != first && ai->ai_canonname == first->ai_canonname) {
            ok(75, 0);
            break;
        }
        found = 0;
        saddr = (struct sockaddr_in *) ai->ai_addr;
        addr = saddr->sin_addr;
        for (i = 0; host->h_addr_list[i] != NULL; i++)
            if (memcmp(&addr, host->h_addr_list[i], host->h_length) == 0)
                found = 1;
        if (!found) {
            ok(75, 0);
            break;
        }
    }
    if (found)
        ok(75, 1);
    test_freeaddrinfo(ai);

    return 0;
}
