/*
 * kadmin/v5server/srv_acl.c
 *
 * Copyright 1995 by the Massachusetts Institute of Technology.
 * All Rights Reserved.
 *
 * Export of this software from the United States of America may
 *   require a specific license from the United States Government.
 *   It is the responsibility of any person or organization contemplating
 *   export to obtain such a license before exporting.
 *
 * WITHIN THAT CONSTRAINT, permission to use, copy, modify, and
 * distribute this software and its documentation for any purpose and
 * without fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright notice and
 * this permission notice appear in supporting documentation, and that
 * the name of M.I.T. not be used in advertising or publicity pertaining
 * to distribution of the software without specific, written prior
 * permission.  M.I.T. makes no representations about the suitability of
 * this software for any purpose.  It is provided "as is" without express
 * or implied warranty.
 *
 */

/*
 * srv_acl.c - Handle Kerberos ACL related functions.
 */
#include <stdio.h>
#include <sys/param.h>
#include <sys/signal.h>
#include "k5-int.h"
#include "com_err.h"
#include "kadm5_defs.h"

typedef struct _acl_op_table {
    char	ao_op;
    krb5_int32	ao_mask;
} aop_t;

typedef struct _acl_entry {
    struct _acl_entry	*ae_next;
    char		*ae_name;
    int			ae_name_bad;
    krb5_principal	ae_principal;
    krb5_int32		ae_op_allowed;
} aent_t;

static const aop_t acl_op_table[] = {
    { 'a',	ACL_ADD_PRINCIPAL },
    { 'd',	ACL_DELETE_PRINCIPAL },
    { 'e',	ACL_EXTRACT },
    { 'm',	ACL_MODIFY_PRINCIPAL },
    { 'c',	ACL_CHANGEPW },
    { 'o',	ACL_CHANGE_OWN_PW },
    { 'i',	ACL_INQUIRE },
    { 'p',	ACL_PRINCIPAL_MASK },
    { 'w',	ACL_PASSWD_MASK },
    { 'x',	ACL_ALL_MASK },
    { '*',	ACL_ALL_MASK },
    { '\0',	0 }
};

static aent_t	*acl_list_head = (aent_t *) NULL;
static aent_t	*acl_list_tail = (aent_t *) NULL;

static const char *acl_acl_file = (char *) NULL;
static int acl_inited = 0;
static int acl_debug_level = 0;
/*
 * This is the catchall entry.  If nothing else appropriate is found, or in
 * the case where the ACL file is not present, this entry controls what can
 * be done.  The default is that everybody can change their own password.
 */
static const char *acl_catchall_entry = "* o";

static const char *acl_line2long_msg = "%s: line %d too long, truncated\n";
static const char *acl_op_bad_msg = "Unrecognized ACL operation '%c' in %s\n";
static const char *acl_syn_err_msg = "%s: syntax error at line %d <%10s...>\n";
static const char *acl_cantopen_msg = "cannot open ACL file";

/*
 * acl_get_line()	- Get a line from the ACL file.
 */
static char *
acl_get_line(fp, lnp)
    FILE	*fp;
    int		*lnp;
{
    int		i, domore;
    static char acl_buf[BUFSIZ];

    for (domore = 1; domore && !feof(fp); ) {
	/* Copy in the line */
	for (i=0;
	     ((i<BUFSIZ) &&
	      (!feof(fp)) &&
	      ((acl_buf[i] = fgetc(fp)) != '\n'));
	     i++);
	acl_buf[i] = '\0';

	/* Check if we exceeded our buffer size */
	if ((i == BUFSIZ) && (!feof(fp)) && (acl_buf[i] != '\n')) {
	    fprintf(stderr, acl_line2long_msg, acl_acl_file, *lnp);
	    while (fgetc(fp) != '\n');
	}
	if (acl_buf[0] == EOF)	/* ptooey */
	    acl_buf[0] = '\0';
	else
	    (*lnp)++;
	if ((acl_buf[0] != '#') && (acl_buf[0] != '\0'))
	    domore = 0;
    }
    if (domore || (strlen(acl_buf) == 0))
	return((char *) NULL);
    else
	return(acl_buf);
}

/*
 * acl_parse_line()	- Parse the contents of an ACL line.
 */
static aent_t *
acl_parse_line(lp)
    char *lp;
{
    static char acle_principal[BUFSIZ];
    static char acle_ops[BUFSIZ];
    aent_t	*acle;
    char	*op;
    int		t, found, opok;

    DPRINT(DEBUG_CALLS, acl_debug_level,
	   ("* acl_parse_line(line=%20s)\n", lp));
    /*
     * Format is very simple:
     *	entry ::=	<whitespace> <principal> <whitespace> <opstring>
     */
    acle = (aent_t *) NULL;
    if (sscanf(lp, "%s %s", acle_principal, acle_ops) == 2) {
	acle = (aent_t *) malloc(sizeof(aent_t));
	if (acle) {
	    acle->ae_next = (aent_t *) NULL;
	    acle->ae_op_allowed = (krb5_int32) 0;
	    opok = 1;
	    for (op=acle_ops; *op; op++) {
		char rop;

		rop = (isupper(*op)) ? tolower(*op) : *op;
		found = 0;
		for (t=0; acl_op_table[t].ao_op; t++) {
		    if (rop == acl_op_table[t].ao_op) {
			found = 1;
			if (rop == *op)
			    acle->ae_op_allowed |= acl_op_table[t].ao_mask;
			else
			    acle->ae_op_allowed &= ~acl_op_table[t].ao_mask;
		    }
		}
		if (!found) {
		    fprintf(stderr, acl_op_bad_msg, *op, lp);
		    opok = 0;
		}
	    }
	    if (opok) {
		acle->ae_name = (char *) malloc(strlen(acle_principal)+1);
		if (acle->ae_name) {
		    strcpy(acle->ae_name, acle_principal);
		    acle->ae_principal = (krb5_principal) NULL;
		    acle->ae_name_bad = 0;
		    DPRINT(DEBUG_ACL, acl_debug_level,
			   ("A ACL entry %s -> opmask %x\n",
			    acle->ae_name, acle->ae_op_allowed));
		}
		else {
		    free(acle);
		    acle = (aent_t *) NULL;
		}
	    }
	    else {
		free(acle);
		acle = (aent_t *) NULL;
	    }
	}
    }
    DPRINT(DEBUG_CALLS, acl_debug_level,
	   ("X acl_parse_line() = %x\n", (long) acle));
    return(acle);
}

/*
 * acl_free_entries()	- Free all ACL entries.
 */
static void
acl_free_entries()
{
    aent_t	*ap;

    DPRINT(DEBUG_CALLS, acl_debug_level, ("* acl_free_entries()\n"));
    for (ap=acl_list_head; ap; ap = ap->ae_next) {
	if (ap->ae_name)
	    free(ap->ae_name);
	if (ap->ae_principal)
	    krb5_free_principal((krb5_context) NULL, ap->ae_principal);
	free(ap);
    }
    acl_list_head = acl_list_tail = (aent_t *) NULL;
    acl_inited = 0;
    DPRINT(DEBUG_CALLS, acl_debug_level, ("X acl_free_entries()\n"));
}

/*
 * acl_load_acl_file()	- Open and parse the ACL file.
 */
static int
acl_load_acl_file()
{
    FILE 	*afp;
    char 	*alinep;
    aent_t	**aentpp;
    int		alineno;
    int		retval = 1;

    DPRINT(DEBUG_CALLS, acl_debug_level, ("* acl_load_acl_file()\n"));
    /* Open the ACL file for read */
    if (afp = fopen(acl_acl_file, "r")) {
	alineno = 1;
	aentpp = &acl_list_head;

	/* Get a non-comment line */
	while (alinep = acl_get_line(afp, &alineno)) {
	    /* Parse it */
	    *aentpp = acl_parse_line(alinep);
	    /* If syntax error, then fall out */
	    if (!*aentpp) {
		fprintf(stderr, acl_syn_err_msg,
			acl_acl_file, alineno, alinep);
		retval = 0;
		break;
	    }
	    acl_list_tail = *aentpp;
	    aentpp = &(*aentpp)->ae_next;
	}
	if (*aentpp = acl_parse_line(acl_catchall_entry)) {
	    acl_list_tail = *aentpp;
	}
	else {
	    retval = 0;
	    DPRINT(DEBUG_OPERATION, acl_debug_level,
		   ("> catchall acl entry (%s) load failed\n",
		    acl_catchall_entry));
	}
	fclose(afp);
    }
    else {
	com_err(acl_acl_file, errno, acl_cantopen_msg);
	if (acl_list_head = acl_parse_line(acl_catchall_entry)) {
	    acl_list_tail = acl_list_head;
	}
	else {
	    retval = 0;
	    DPRINT(DEBUG_OPERATION, acl_debug_level,
		   ("> catchall acl entry (%s) load failed\n",
		    acl_catchall_entry));
	}
    }

    if (!retval) {
	acl_free_entries();
    }
    DPRINT(DEBUG_CALLS, acl_debug_level,
	   ("X acl_load_acl_file() = %d\n", retval));
    return(retval);
}

/*
 * acl_reload_acl_file()	- Reload the acl file.
 */
static krb5_sigtype
acl_reload_acl_file()
{
    DPRINT(DEBUG_CALLS, acl_debug_level, ("* acl_reload_acl_file()\n"));
    acl_free_entries();
    acl_inited = acl_load_acl_file();
    DPRINT(DEBUG_CALLS, acl_debug_level, ("X acl_reload_acl_file()\n"));
}

/*
 * acl_match_data()	- See if two data entries match.
 *
 * Wildcarding is only supported for a whole component.
 */
static krb5_boolean
acl_match_data(e1, e2)
    krb5_data	*e1, *e2;
{
    krb5_boolean	retval;

    DPRINT(DEBUG_CALLS, acl_debug_level, 
	   ("* acl_match_entry(%s, %s)\n", e1->data, e2->data));
    retval = 0;
    if (!strncmp(e1->data, "*", e1->length) ||
	!strncmp(e2->data, "*", e2->length)) {
	retval = 1;
    }
    else {
	if ((e1->length == e2->length) &&
	    (!strncmp(e1->data, e2->data, e1->length)))
	    retval = 1;
    }
    DPRINT(DEBUG_CALLS, acl_debug_level, ("X acl_match_entry()=%d\n",retval));
    return(retval);
}

/*
 * acl_find_entry()	- Find a matching entry.
 */
static aent_t *
acl_find_entry(kcontext, principal)
    krb5_context	kcontext;
    krb5_principal	principal;
{
    aent_t		*entry;
    krb5_error_code	kret;
    int			i;
    int			matchgood;

    DPRINT(DEBUG_CALLS, acl_debug_level, ("* acl_find_entry()\n"));
    for (entry=acl_list_head; entry; entry = entry->ae_next) {
	if (!strcmp(entry->ae_name, "*")) {
	    DPRINT(DEBUG_ACL, acl_debug_level, ("A wildcard ACL match\n"));
	    break;
	}
	if (!entry->ae_principal && !entry->ae_name_bad) {
	    kret = krb5_parse_name(kcontext,
				   entry->ae_name,
				   &entry->ae_principal);
	    if (kret)
		entry->ae_name_bad = 1;
	}
	if (entry->ae_name_bad) {
	    DPRINT(DEBUG_ACL, acl_debug_level,
		   ("A Bad ACL entry %s\n", entry->ae_name));
	    continue;
	}
	matchgood = 0;
	if (acl_match_data(&entry->ae_principal->realm,
			   &principal->realm) &&
	    (entry->ae_principal->length == principal->length)) {
	    matchgood = 1;
	    for (i=0; i<principal->length; i++) {
		if (!acl_match_data(&entry->ae_principal->data[i],
				    &principal->data[i])) {
		    matchgood = 0;
		    break;
		}
	    }
	}
	if (matchgood)
	    break;
    }
    DPRINT(DEBUG_CALLS, acl_debug_level, ("X acl_find_entry()=%x\n",entry));
    return(entry);
}

/*
 * acl_init()	- Initialize ACL context.
 */
krb5_error_code
acl_init(kcontext, debug_level, acl_file)
    krb5_context	kcontext;
    int			debug_level;
    char		*acl_file;
{
    krb5_error_code	kret;
#if	POSIX_SIGNALS
    struct sigaction	s_action;
#endif	/* POSIX_SIGNALS */

    kret = 0;
    acl_debug_level = debug_level;
    DPRINT(DEBUG_CALLS, acl_debug_level,
	   ("* acl_init(afile=%s)\n",
	    ((acl_file) ? acl_file : "(null)")));
    acl_acl_file = (acl_file) ? acl_file : (char *) KRB5_DEFAULT_ADMIN_ACL;
    acl_inited = acl_load_acl_file();
#if	POSIX_SIGNALS
    (void) sigemptyset(&s_action.sa_mask);
    s_action.sa_flags = 0;
    s_action.sa_handler = acl_reload_acl_file;
    (void) sigaction(SIGALRM, &s_action, (struct sigaction *) NULL);
#else	/* POSIX_SIGNALS */
    signal(SIGHUP, acl_reload_acl_file);
#endif	/* POSIX_SIGNALS */
    DPRINT(DEBUG_CALLS, acl_debug_level, ("X acl_init() = %d\n", kret));
    return(kret);
}

/*
 * acl_finish	- Terminate ACL context.
 */
void
acl_finish(kcontext, debug_level)
    krb5_context	kcontext;
    int			debug_level;
{
    DPRINT(DEBUG_CALLS, acl_debug_level, ("* acl_finish()\n"));
    acl_free_entries();
    DPRINT(DEBUG_CALLS, acl_debug_level, ("X acl_finish()\n"));
}

/*
 * acl_op_permitted()	- Is this operation permitted for this principal?
 */
krb5_boolean
acl_op_permitted(kcontext, principal, opmask)
    krb5_context	kcontext;
    krb5_principal	principal;
    krb5_int32		opmask;
{
    krb5_boolean	retval;
    aent_t		*aentry;

    DPRINT(DEBUG_CALLS, acl_debug_level, ("* acl_op_permitted()\n"));
    retval = 0;
    if (aentry = acl_find_entry(kcontext, principal)) {
	if ((aentry->ae_op_allowed & opmask) == opmask)
	    retval = 1;
    }
    DPRINT(DEBUG_CALLS, acl_debug_level, ("X acl_op_permitted()=%d\n",
					  retval));
    return(retval);
}
