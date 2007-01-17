/* $Id: setenv-t.c 7168 2005-04-10 08:52:48Z rra $ */
/* setenv test suite. */

/* Copyright (c) 2004, 2005, 2006
       by Internet Systems Consortium, Inc. ("ISC")
   Copyright (c) 1991, 1994, 1995, 1996, 1997, 1998, 1999, 2000, 2001,
       2002, 2003 by The Internet Software Consortium and Rich Salz

   This code is derived from software contributed to the Internet Software
   Consortium by Rich Salz.

   Permission to use, copy, modify, and distribute this software for any
   purpose with or without fee is hereby granted, provided that the above
   copyright notice and this permission notice appear in all copies.

   THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
   REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
   MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY
   SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
   WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
   ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
   OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. */

#include <config.h>
#include <system.h>

#include <errno.h>

#include <tests/libtest.h>
#include <util/util.h>

int test_setenv(const char *name, const char *value, int overwrite);

static const char test_var[] = "SETENV_TEST";
static const char test_value1[] = "Do not taunt Happy Fun Ball.";
static const char test_value2[] = "Do not use Happy Fun Ball on concrete.";

int
main(void)
{
    if (getenv(test_var))
        die("%s already in the environment!", test_var);

    test_init(8);

    ok(1, test_setenv(test_var, test_value1, 0) == 0);
    ok_string(2, test_value1, getenv(test_var));
    ok(3, test_setenv(test_var, test_value2, 0) == 0);
    ok_string(4, test_value1, getenv(test_var));
    ok(5, test_setenv(test_var, test_value2, 1) == 0);
    ok_string(6, test_value2, getenv(test_var));
    ok(7, test_setenv(test_var, "", 1) == 0);
    ok_string(8, "", getenv(test_var));

    return 0;
}