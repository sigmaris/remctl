/*
 * Declarations for the remctl PECL extension for PHP
 *
 * Written by Andrew Mortensen <admorten@umich.edu>, 2008
 * Copyright 2008 Andrew Mortensen <admorten@umich.edu>
 * Copyright 2008 Board of Trustees, Leland Stanford Jr. University
 *
 * See LICENSE for licensing terms.
 */

#ifndef PHP_REMCTL_H
#define PHP_REMCTL_H 1

/* This should be the same version as the overall remctl package. */
#define PHP_REMCTL_VERSION  "2.13"
#define PHP_REMCTL_EXTNAME  "remctl"
#define PHP_REMCTL_RES_NAME "remctl_resource"

PHP_MINIT_FUNCTION(remctl);
PHP_FUNCTION(remctl);
PHP_FUNCTION(remctl_new);
PHP_FUNCTION(remctl_open);
PHP_FUNCTION(remctl_command);
PHP_FUNCTION(remctl_output);
PHP_FUNCTION(remctl_error);
PHP_FUNCTION(remctl_close);

extern zend_module_entry remctl_module_entry;

#endif /* PHP_REMCTL_H */
