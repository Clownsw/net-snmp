/*
 * agent_registry.c
 *
 * Maintain a registry of MIB subtrees, together
 *   with related information regarding mibmodule, sessions, etc
 */

#define IN_SNMP_VARS_C

#include <net-snmp/net-snmp-config.h>
#include <signal.h>
#if HAVE_STRING_H
#include <string.h>
#endif
#if HAVE_STDLIB_H
#include <stdlib.h>
#endif
#include <sys/types.h>
#include <stdio.h>
#include <fcntl.h>
#if HAVE_WINSOCK_H
#include <winsock.h>
#endif
#if TIME_WITH_SYS_TIME
# ifdef WIN32
#  include <sys/timeb.h>
# else
#  include <sys/time.h>
# endif
# include <time.h>
#else
# if HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif
#if HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif

#if HAVE_DMALLOC_H
#include <dmalloc.h>
#endif

#include "mibincl.h"
#include <net-snmp/snmp_client.h>
#include <net-snmp/default_store.h>
#include <net-snmp/agent/ds_agent.h>
#include <net-snmp/callback.h>
#include <net-snmp/agent/agent_callbacks.h>
#include <net-snmp/agent/agent_registry.h>
#include <net-snmp/snmp_alarm.h>
#include <net-snmp/snmp_secmod.h>
#include <net-snmp/vacm.h>

#include "snmpd.h"
#include "mibgroup/struct.h"
#include "net-snmp/agent/old_api.h"
#include "net-snmp/agent/null.h"
#include "net-snmp/agent/table.h"
#include "net-snmp/agent/table_iterator.h"
#include "mib_module_includes.h"

#ifdef USING_AGENTX_SUBAGENT_MODULE
#include "agentx/subagent.h"
#include "agentx/client.h"
#endif

subtree_context_cache *context_subtrees = NULL;

subtree_context_cache *get_top_context_cache(void) {
    return context_subtrees;
}

struct subtree *
find_first_subtree(const char *context_name) {
    subtree_context_cache *ptr;
    if (!context_name)
        context_name = "";
    DEBUGMSGTL(("find_first_subtree","looking for subtree for context: \"%s\"\n", context_name));
    for(ptr = context_subtrees; ptr; ptr = ptr->next) {
        if (strcmp(ptr->context_name, context_name) == 0) {
            DEBUGMSGTL(("find_first_subtree","found one for: \"%s\"\n", context_name));
            return ptr->first_subtree;
        }
    }
    DEBUGMSGTL(("find_first_subtree","Didn't find a subtree for: \"%s\"\n", context_name));
    return NULL;
}

struct subtree *
add_subtree(struct subtree *new_tree, const char *context_name) {
    subtree_context_cache *ptr = SNMP_MALLOC_TYPEDEF(subtree_context_cache);
    if (!context_name)
        context_name = "";

    if (!ptr)
        return NULL;

    DEBUGMSGTL(("add_subtree","adding subtree for context: \"%s\"\n", context_name));
    ptr->next = context_subtrees;
    ptr->first_subtree = new_tree;
    ptr->context_name = strdup(context_name);
    context_subtrees = ptr;
    return ptr->first_subtree;
}

struct subtree *
replace_first_subtree(struct subtree *new_tree, const char *context_name) {
    subtree_context_cache *ptr;
    if (!context_name)
        context_name = "";
    for(ptr = context_subtrees; ptr; ptr = ptr->next) {
        if (strcmp(ptr->context_name, context_name) == 0) {
            ptr->first_subtree = new_tree;
            return ptr->first_subtree;
        }
    }
    return add_subtree(new_tree, context_name);
}



int tree_compare(const struct subtree *ap, const struct subtree *bp)
{
  return snmp_oid_compare(ap->name,ap->namelen,bp->name,bp->namelen);
}

void
join_subtree(struct subtree *root)
{
  struct subtree *s, *tmp, *c, *d;

  while (root != NULL) {
    s = root->next;
    while (s != NULL && root->reginfo == s->reginfo) {
      tmp = s->next;
      DEBUGMSGTL(("join_subtree", "root start "));
      DEBUGMSGOID(("join_subtree", root->start, root->start_len));
      DEBUGMSG(("join_subtree", " (original end "));
      DEBUGMSGOID(("join_subtree", root->end, root->end_len));
      DEBUGMSG(("join_subtree", ")\n"));
      DEBUGMSGTL(("join_subtree", "  joining to "));
      DEBUGMSGOID(("join_subtree", s->start, s->start_len));
      memcpy(root->end, s->end, s->end_len * sizeof(oid));
      root->end_len = s->end_len;
      for (c = root; c != NULL; c = c->children) {
	c->next = s->next;
      }
      for (c = s; c != NULL; c = c->children) {
	c->prev = root;
      }
      DEBUGMSG(("join_subtree", " so new end "));
      DEBUGMSGOID(("join_subtree", root->end, root->end_len));
      DEBUGMSG(("join_subtree", "\n"));
      /*  Probably need to free children too?  */
      for (c = s->children; c != NULL; c = d) {
	d = c->children;
	free_subtree(c);
      }
      free_subtree(s);
      s = tmp;
    }
    root = root->next;
  }
}


	/*
	 *  Split the subtree into two at the specified point,
	 *    returning the new (second) subtree
	 */
struct subtree *
split_subtree(struct subtree *current, oid name[], int name_len)
{
    struct variable *vp = NULL;
    struct subtree *new_sub, *ptr;
    int i = 0, rc = 0, rc2 = 0;
    size_t common_len = 0;
    char *cp;

    if ( snmp_oid_compare(name, name_len,
			  current->end, current->end_len) > 0 )
	return NULL;	/* Split comes after the end of this subtree */

    new_sub = (struct subtree *)malloc(sizeof(struct subtree));
    if ( new_sub == NULL )
	return NULL;
    memcpy(new_sub, current, sizeof(struct subtree));

	/* Set up the point of division */
    memcpy(current->end,   name, name_len*sizeof(oid));
    memcpy(new_sub->start, name, name_len*sizeof(oid));
    current->end_len   = name_len;
    new_sub->start_len = name_len;

	/*
	 * Split the variables between the two new subtrees
	 */
    i = current->variables_len;
    current->variables_len = 0;

    for (vp = current->variables; i > 0; i--) {
		/* Note that the variable "name" field omits
		   the prefix common to the whole registration,
		   hence the strange comparison here */

	rc = snmp_oid_compare(vp->name, vp->namelen,
			      name     + current->namelen, 
			      name_len - current->namelen);
	
	if (name_len - current->namelen > vp->namelen) {
	    common_len = vp->namelen;
	} else {
	    common_len = name_len - current->namelen;
	}

	rc2 = snmp_oid_compare(vp->name, common_len,
			       name + current->namelen, common_len);

	if (rc >= 0)
	    break;	/* All following variables belong to the second subtree */

	current->variables_len++;
	if (rc2 < 0) {
	    new_sub->variables_len--;
	    cp = (char *)new_sub->variables;
	    new_sub->variables = (struct variable *)(cp + new_sub->variables_width);
	}
	vp = (struct variable *)((char *)vp + current->variables_width);
    }

	/* Delegated trees should retain their variables regardless */
    if ( current->variables_len > 0 &&
		IS_DELEGATED((u_char)current->variables[0].type)) {
	new_sub->variables_len = 1;
	new_sub->variables     = current->variables;
    }

	/* Propogate this split down through any children */
    if ( current->children )
	new_sub->children = split_subtree(current->children, name, name_len);

	/* Retain the correct linking of the list */
    for ( ptr = current ; ptr != NULL ; ptr=ptr->children )
          ptr->next = new_sub;
    for ( ptr = new_sub ; ptr != NULL ; ptr=ptr->children )
          ptr->prev = current;
    for ( ptr = new_sub->next ; ptr != NULL ; ptr=ptr->children )
          ptr->prev = new_sub;

    /* retain original APIv2 registration information */
    new_sub->reginfo = current->reginfo;

    return new_sub;
}

int
load_subtree( struct subtree *new_sub, const char *context_name )
{
    struct subtree *tree1, *tree2, *new2;
    struct subtree *prev, *next;
    int res, rc = 0;

    if ( new_sub == NULL )
	return MIB_REGISTERED_OK;	/* Degenerate case */

		/*
		 * Find the subtree that contains the start of 
		 *  the new subtree (if any)...
		 */
    tree1 = find_subtree( new_sub->start, new_sub->start_len, NULL,
                          context_name );
		/*
		 * ...and the subtree that follows the new one
		 *	(NULL implies this is the final region covered)
		 */  
    if ( tree1 == NULL )
        tree2 = find_subtree_next( new_sub->start, new_sub->start_len, NULL,
                                   context_name);
    else
	tree2 = tree1->next;


	/*
	 * Handle new subtrees that start in virgin territory.
	 */
    if ( tree1 == NULL ) {
	new2 = NULL;
		/* Is there any overlap with later subtrees ? */
	if ( tree2 && snmp_oid_compare( new_sub->end, new_sub->end_len,
					tree2->start, tree2->start_len ) > 0 )
	    new2 = split_subtree( new_sub, tree2->start, tree2->start_len );
		/*
		 * Link the new subtree (less any overlapping region)
		 *  with the list of existing registrations
		 */
	if ( tree2 ) {
	    new_sub->prev = tree2->prev;
	    tree2->prev       = new_sub;
	}
	else
	    new_sub->prev = find_subtree_previous( new_sub->start, new_sub->start_len, NULL, context_name );

	if ( new_sub->prev )
	    new_sub->prev->next = new_sub;
	else
	    replace_first_subtree(new_sub, context_name);

	new_sub->next     = tree2;

		/*
		 * If there was any overlap,
		 *  recurse to merge in the overlapping region
		 *  (including anything that may follow the overlap)
		 */
	if ( new2 )
	    return load_subtree( new2, context_name );
    }

    else {
	/*
	 *  If the new subtree starts *within* an existing registration
	 *    (rather than at the same point as it), then split the
	 *    existing subtree at this point.
	 */
	if ( snmp_oid_compare( new_sub->start, new_sub->start_len, 
			       tree1->start,   tree1->start_len) != 0 )
	    tree1 = split_subtree(tree1, new_sub->start, new_sub->start_len);

	if ( tree1 == NULL )
	    return MIB_REGISTRATION_FAILED;

	/*  Now consider the end of this existing subtree:
	 *	If it matches the new subtree precisely,
	 *	  simply merge the new one into the list of children
	 *	If it includes the whole of the new subtree,
	 *	  split it at the appropriate point, and merge again
	 *
	 *	If the new subtree extends beyond this existing region,
	 *	  split it, and recurse to merge the two parts.
	 */
	rc = snmp_oid_compare(new_sub->end, new_sub->end_len, 
			      tree1->end, tree1->end_len);

	 switch (rc)  {

		case -1:	/* Existing subtree contains new one */
			(void) split_subtree(tree1,
					     new_sub->end, new_sub->end_len);
			/* Fall Through */

		case  0:	/* The two trees match precisely */
			/*
			 * Note: This is the only point where the original
			 *	 registration OID ("name") is used
			 */
			prev = NULL;
			next = tree1;
			while ( next && next->namelen > new_sub->namelen ) {
				prev = next;
				next = next->children;
			}
			while ( next && next->namelen == new_sub->namelen &&
					next->priority < new_sub->priority ) {
				prev = next;
				next = next->children;
			}
			if ( next &&	next->namelen  == new_sub->namelen &&
					next->priority == new_sub->priority )
			   return MIB_DUPLICATE_REGISTRATION;

			if ( prev ) {
			    new_sub->children = next;
			    prev->children    = new_sub;
			    new_sub->prev = prev->prev;
			    new_sub->next = prev->next;
			}
			else {
			    new_sub->children = next;
			    new_sub->prev = next->prev;
			    new_sub->next = next->next;

			    for ( next = new_sub->next ;
			    	  next != NULL ;
				  next = next->children )
					next->prev = new_sub;

			    for ( prev = new_sub->prev ;
			    	  prev != NULL ;
				  prev = prev->children )
					prev->next = new_sub;
			}
			break;

		case  1:	/* New subtree contains the existing one */
	    		new2 = split_subtree( new_sub,
					tree1->end, tree1->end_len);
			res = load_subtree( new_sub, context_name );
			if ( res != MIB_REGISTERED_OK ) {
			    free_subtree(new2);
			    return res;
			}
			return load_subtree( new2, context_name );

	 }

    }
    return 0;
}

int
register_mib_context2(const char *moduleName,
	     struct variable *var,
	     size_t varsize,
	     size_t numvars,
	     oid *mibloc,
	     size_t mibloclen,
	     int priority,
	     int range_subid,
	     oid range_ubound,
	     struct snmp_session *ss,
	     const char *context,
	     int timeout,
	     int flags,
             handler_registration *reginfo,
	     int perform_callback)
{
  struct subtree *subtree, *sub2;
  int res, i;
  struct register_parameters reg_parms;
  
  subtree = (struct subtree *) malloc(sizeof(struct subtree));
  if (subtree == NULL) {
    return MIB_REGISTRATION_FAILED;
  }
  memset(subtree, 0, sizeof(struct subtree));

  DEBUGMSGTL(("register_mib", "registering \"%s\" at ", moduleName));
  DEBUGMSGOIDRANGE(("register_mib", mibloc, mibloclen, range_subid, range_ubound));
  DEBUGMSG(("register_mib","\n"));
    
	/*
	 * Create the new subtree node being registered
	 */
  memcpy(subtree->name, mibloc, mibloclen*sizeof(oid));
  subtree->namelen = (u_char) mibloclen;
  memcpy(subtree->start, mibloc, mibloclen*sizeof(oid));
  subtree->start_len = (u_char) mibloclen;
  memcpy(subtree->end, mibloc, mibloclen*sizeof(oid));
  subtree->end[ mibloclen-1 ]++;	/* XXX - or use 'variables' info ? */
  subtree->end_len = (u_char) mibloclen;
  memcpy(subtree->label, moduleName, strlen(moduleName)+1);
  if (var) {
    subtree->variables = (struct variable *) malloc(varsize*numvars);
    if (subtree->variables == NULL) {
      free(subtree);
      return MIB_REGISTRATION_FAILED;
    }
    memcpy(subtree->variables, var, numvars*varsize);
    subtree->variables_len = numvars;
    subtree->variables_width = varsize;
  }
  subtree->priority = priority;
  subtree->timeout  = timeout;
  subtree->range_subid = range_subid;
  subtree->range_ubound = range_ubound;
  subtree->session = ss;
  subtree->reginfo = reginfo;
  subtree->flags = (u_char)flags;  /*  used to identify instance oids  */
  subtree->flags |= SUBTREE_ATTACHED;
  res = load_subtree(subtree, context);

	/*
	 * If registering a range,
	 *   use the first subtree as a template
	 *   for the rest of the range
	 */
  if ((res == MIB_REGISTERED_OK) && (range_subid != 0)) {
    for (i = mibloc[range_subid - 1] + 1; i <= (int)range_ubound; i++) {
	sub2 = (struct subtree *)malloc(sizeof(struct subtree));

	if (sub2 == NULL) {
	    unregister_mib_context(mibloc, mibloclen, priority,
				   range_subid, range_ubound, context);
	    return MIB_REGISTRATION_FAILED;
	}

	memcpy(sub2, subtree, sizeof(struct subtree));

	/*  Note: have to deep-copy sub2->variables, otherwise it will
	    be free()d more than once.  This is kind of inefficient.  */
	
	if (subtree->variables != NULL) {
	  sub2->variables = (struct variable *)malloc(varsize * numvars);
	  if (sub2->variables == NULL) {
	    free(sub2);
	    unregister_mib_context(mibloc, mibloclen, priority,
				   range_subid, range_ubound, context);
	    return MIB_REGISTRATION_FAILED;
	  }
	  memcpy(sub2->variables, var, numvars * varsize);
	}

	/*  Note: have to deep copy reginfo here also.  */
	if (subtree->reginfo != NULL) {
	  sub2->reginfo = snmp_handler_registration_dup(subtree->reginfo);
	  if (sub2->reginfo == NULL) {
	    free(sub2);
	    unregister_mib_context(mibloc, mibloclen, priority,
				   range_subid, range_ubound, context);
	    return MIB_REGISTRATION_FAILED;
	  }
	}

	sub2->name[range_subid - 1] = i;
	sub2->start[range_subid - 1] = i;
	sub2->end[range_subid - 1] = i;		/* XXX - ???? */
	res = load_subtree(sub2, context);
	sub2->flags |= SUBTREE_ATTACHED;
	if (res != MIB_REGISTERED_OK) {
	    unregister_mib_context( mibloc, mibloclen, priority,
				  range_subid, range_ubound, context);
	    return MIB_REGISTRATION_FAILED;
	}
    }
  } else if (res == MIB_DUPLICATE_REGISTRATION ||
	     res == MIB_REGISTRATION_FAILED) {
      free_subtree(subtree);
  }

  if (perform_callback) {
      reg_parms.name = mibloc;
      reg_parms.namelen = mibloclen;
      reg_parms.priority = priority;
      reg_parms.range_subid  = range_subid;
      reg_parms.range_ubound = range_ubound;
      reg_parms.timeout = timeout;
      reg_parms.flags = (u_char)flags;

      /*  Should this really be called if the registration hasn't actually 
	  succeeded?  */

      snmp_call_callbacks(SNMP_CALLBACK_APPLICATION,
			  SNMPD_CALLBACK_REGISTER_OID, &reg_parms);
  }
  
  return res;
}

/*  Reattach a particular node.  */

static void
register_mib_reattach_node(struct subtree *s)
{
  if ((s != NULL) && (s->namelen > 1) && !(s->flags & SUBTREE_ATTACHED)) {
      struct register_parameters reg_parms;
      /* only do registrations that are not the top level nodes */
      /* XXX: do this better */
      reg_parms.name = s->name;
      reg_parms.namelen = s->namelen;
      reg_parms.priority = s->priority;
      reg_parms.range_subid  = s->range_subid;
      reg_parms.range_ubound = s->range_ubound;
      reg_parms.timeout = s->timeout;
      reg_parms.flags = s->flags;
      snmp_call_callbacks(SNMP_CALLBACK_APPLICATION,
			  SNMPD_CALLBACK_REGISTER_OID, &reg_parms);
      s->flags |= SUBTREE_ATTACHED;
  }
}

/*  Call callbacks to reattach all our nodes.  */

void
register_mib_reattach(void)
{
  struct subtree *s, *t;
  subtree_context_cache *ptr;

  for(ptr = context_subtrees; ptr; ptr = ptr->next) {
      for (s = ptr->first_subtree; s != NULL; s = s->next) {
          register_mib_reattach_node(s);
          for (t = s->children; t != NULL; t = t->children) {
              register_mib_reattach_node(t);
          }
      }
  }
}

/*  Mark a node as detached.  */

static void
register_mib_detach_node(struct subtree *s)
{
  if (s != NULL) {
    s->flags = s->flags & ~SUBTREE_ATTACHED;
  }
}

/*  Mark all our registered OIDs as detached.  This is only really
    useful for subagent protocols, when a connection is lost or
    something.  */

void
register_mib_detach(void)
{
  struct subtree *s, *t;
  subtree_context_cache *ptr;
  for(ptr = context_subtrees; ptr; ptr = ptr->next) {
      for (s = ptr->first_subtree; s != NULL; s = s->next) {
          register_mib_detach_node(s);
          for (t = s->children; t != NULL; t = t->children) {
              register_mib_detach_node(t);
          }
      }
  }
}

int
register_mib_context(const char *moduleName,
                     struct variable *var,
                     size_t varsize,
                     size_t numvars,
                     oid *mibloc,
                     size_t mibloclen,
                     int priority,
                     int range_subid,
                     oid range_ubound,
                     struct snmp_session *ss,
                     const char *context,
                     int timeout,
                     int flags) {
    return register_old_api(moduleName, var, varsize, numvars, mibloc,
                            mibloclen, priority,
                            range_subid, range_ubound,
                            ss, context, timeout, flags);
}

int
register_mib_range(const char *moduleName,
	     struct variable *var,
	     size_t varsize,
	     size_t numvars,
	     oid *mibloc,
	     size_t mibloclen,
	     int priority,
	     int range_subid,
	     oid range_ubound,
	     struct snmp_session *ss)
{
  return register_mib_context( moduleName, var, varsize, numvars,
				mibloc, mibloclen, priority,
				range_subid, range_ubound, ss, "", -1, 0);
}

int
register_mib_priority(const char *moduleName,
	     struct variable *var,
	     size_t varsize,
	     size_t numvars,
	     oid *mibloc,
	     size_t mibloclen,
	     int priority)
{
  return register_mib_range( moduleName, var, varsize, numvars,
				mibloc, mibloclen, priority, 0, 0, NULL );
}

int
register_mib(const char *moduleName,
	     struct variable *var,
	     size_t varsize,
	     size_t numvars,
	     oid *mibloc,
	     size_t mibloclen)
{
  return register_mib_priority( moduleName, var, varsize, numvars,
				mibloc, mibloclen, DEFAULT_MIB_PRIORITY );
}

void
unload_subtree( struct subtree *sub, struct subtree *prev)
{
    struct subtree *ptr;

    DEBUGMSGTL(("register_mib", "unload("));
    if (sub != NULL) {
      DEBUGMSGOID(("register_mib", sub->start, sub->start_len));
    } else {
      DEBUGMSG(("register_mib", "[NIL]"));
    }
    DEBUGMSG(("register_mib", ", "));
    if (prev != NULL) {
      DEBUGMSGOID(("register_mib", prev->start, prev->start_len));
    } else {
      DEBUGMSG(("register_mib", "[NIL]"));
    } 
    DEBUGMSG(("register_mib", ")\n"));

    if ( prev != NULL ) {	/* non-leading entries are easy */
	prev->children = sub->children;
	return;
    }
			/* otherwise, we need to amend our neighbours as well */

    if ( sub->children == NULL) {	/* just remove this node completely */
	for (ptr = sub->prev ; ptr ; ptr=ptr->children )
	    ptr->next = sub->next;
	for (ptr = sub->next ; ptr ; ptr=ptr->children )
	    ptr->prev = sub->prev;
	return;
    }
    else {
	for (ptr = sub->prev ; ptr ; ptr=ptr->children )
	    ptr->next = sub->children;
	for (ptr = sub->next ; ptr ; ptr=ptr->children )
	    ptr->prev = sub->children;
	return;
    }
}

int
unregister_mib_context( oid *name, size_t len, int priority,
	     		int range_subid, oid range_ubound, const char* context)
{
  struct subtree *list, *myptr;
  struct subtree *prev, *child;             /* loop through children */
  struct register_parameters reg_parms;

  DEBUGMSGTL(("register_mib", "unregistering "));
  DEBUGMSGOIDRANGE(("register_mib", name, len, range_subid, range_ubound));
  DEBUGMSG(("register_mib","\n"));

  list = find_subtree( name, len, find_first_subtree(context), context );
  if ( list == NULL )
	return MIB_NO_SUCH_REGISTRATION;

  for ( child=list, prev=NULL;  child != NULL;
			 	prev=child, child=child->children ) {
      if (( snmp_oid_compare( child->name, child->namelen, name, len) == 0 )
	  && ( child->priority == priority ))
		break;	/* found it */
  }
  if ( child == NULL )
	return MIB_NO_SUCH_REGISTRATION;

  unload_subtree( child, prev );
  myptr = child;	/* remember this for later */

		/*
		 *  Now handle any occurances in the following subtrees,
		 *	as a result of splitting this range.  Due to the
		 *	nature of the way such splits work, the first
		 * 	subtree 'slice' that doesn't refer to the given
		 *	name marks the end of the original region.
		 *
		 *  This should also serve to register ranges.
		 */

  for ( list = myptr->next ; list != NULL ; list=list->next ) {
  	for ( child=list, prev=NULL;  child != NULL;
			 	      prev=child, child=child->children ) {
	    if (( snmp_oid_compare( child->name, child->namelen,
							name, len) == 0 )
		&& ( child->priority == priority )) {

		    unload_subtree( child, prev );
		    free_subtree( child );
		    break;
	    }
	}
	if ( child == NULL )	/* Didn't find the given name */
	    break;
  }
  free_subtree( myptr );
  
  reg_parms.name = name;
  reg_parms.namelen = len;
  reg_parms.priority = priority;
  reg_parms.range_subid  = range_subid;
  reg_parms.range_ubound = range_ubound;
  reg_parms.flags = 0x00;  /*  this is okay I think  */
  snmp_call_callbacks(SNMP_CALLBACK_APPLICATION, SNMPD_CALLBACK_UNREGISTER_OID,
                      &reg_parms);

  return MIB_UNREGISTERED_OK;
}

int 
unregister_mib_table_row(oid *name, size_t len, int priority,
			 int var_subid, oid range_ubound, const char *context)
{
  struct subtree *list, *myptr;
  struct subtree *prev, *child;             /* loop through children */
  struct register_parameters reg_parms;
  oid range_lbound = name[var_subid - 1];

  DEBUGMSGTL(("register_mib", "unregistering "));
  DEBUGMSGOIDRANGE(("register_mib", name, len, var_subid, range_ubound));
  DEBUGMSG(("register_mib","\n"));

  for (; name[var_subid - 1] <= range_ubound; name[var_subid - 1]++) {
    list = find_subtree(name, len, find_first_subtree(context), context);

    if (list == NULL) {
      continue;
    }

    for (child=list, prev=NULL; child != NULL;
	 prev=child, child=child->children) {

      if ((snmp_oid_compare(child->name, child->namelen, name, len) == 0) &&
	  (child->priority == priority)) {
	break;	/* found it */
      }
    }

    if (child == NULL) {
      continue;
    }

    unload_subtree(child, prev);
    myptr = child;	/* remember this for later */

    for (list = myptr->next; list != NULL; list=list->next) {
      for (child=list, prev=NULL; child != NULL;
	   prev=child, child=child->children) {

	if ((snmp_oid_compare(child->name, child->namelen, name, len) == 0) &&
	    (child->priority == priority)) {
	  unload_subtree(child, prev);
	  free_subtree(child);
	  break;
	}
      }
      if (child == NULL) {	/* Didn't find the given name */
	break;
      }
    }
    free_subtree(myptr);
  }

  name[var_subid - 1] = range_lbound;
  reg_parms.name = name;
  reg_parms.namelen = len;
  reg_parms.priority = priority;
  reg_parms.range_subid  = var_subid;
  reg_parms.range_ubound = range_ubound;
  reg_parms.flags = 0x00;  /*  this is okay I think  */
  snmp_call_callbacks(SNMP_CALLBACK_APPLICATION, SNMPD_CALLBACK_UNREGISTER_OID,
                      &reg_parms);
  
  return 0;
}

int
unregister_mib_range( oid *name, size_t len, int priority,
	     		int range_subid, oid range_ubound)
{
  return unregister_mib_context( name, len, priority, range_subid, range_ubound, "" );
}

int
unregister_mib_priority(oid *name, size_t len, int priority)
{
  return unregister_mib_range( name, len, priority, 0, 0 );
}

int
unregister_mib(oid *name,
	       size_t len)
{
  return unregister_mib_priority( name, len, DEFAULT_MIB_PRIORITY );
}

void
unregister_mibs_by_session (struct snmp_session *ss)
{
  struct subtree *list, *list2;
  struct subtree *child, *prev, *next_child;
  struct register_parameters rp;
  oid namecopy[UCD_REGISTRY_OID_MAX_LEN] = { 0 };
  subtree_context_cache *contextptr;

  DEBUGMSGTL(("register_mib", "unregister_mibs_by_session(%08p) ctxt \"%s\"\n",
	      ss, ss->contextName?ss->contextName:"[NIL]"));

  for (contextptr = get_top_context_cache(); contextptr != NULL;
       contextptr = contextptr->next) {
      for (list = contextptr->first_subtree; list != NULL; list = list2) {
          list2 = list->next;

          for (child=list, prev=NULL;  child != NULL; child=next_child) {
              next_child = child->children;

              if (( (ss->flags & SNMP_FLAGS_SUBSESSION) &&
                    child->session == ss ) ||
                  (!(ss->flags & SNMP_FLAGS_SUBSESSION) && child->session &&
                   child->session->subsession == ss )) {

                  memcpy(namecopy, child->name, child->namelen*sizeof(oid));
                  rp.name         = namecopy;
                  rp.namelen      = child->namelen;
                  rp.priority     = child->priority;
                  rp.range_subid  = child->range_subid;
                  rp.range_ubound = child->range_ubound;
                  rp.timeout      = child->timeout;
                  rp.flags        = child->flags;

		  if (child->reginfo != NULL) {
		    /*  Don't let's free the session pointer just yet!  */
		    child->reginfo->handler->myvoid = NULL;
		    snmp_handler_registration_free(child->reginfo);
		  }

                  unload_subtree(child, prev);
                  free_subtree(child);

                  snmp_call_callbacks(SNMP_CALLBACK_APPLICATION,
                                      SNMPD_CALLBACK_UNREGISTER_OID, &rp);
              } else {
                  prev = child;
              }
          }
      }
      join_subtree(contextptr->first_subtree);
  }
}


struct subtree *
free_subtree(struct subtree *st)
{
  struct subtree *ret = NULL;

  if (st->variables != NULL &&
      snmp_oid_compare(st->name, st->namelen, st->start, st->start_len) == 0) {
    free(st->variables);
    st->variables = NULL;
  }
  if (st->next != NULL) {
    ret = st->next;
  }
  free(st);
  return ret;
}

/* in_a_view: determines if a given snmp_pdu is allowed to see a
   given name/namelen OID pointer
   name         IN - name of var, OUT - name matched
   nameLen      IN -number of sub-ids in name, OUT - subid-is in matched name
   pi           IN - relevant auth info re PDU 
   cvp          IN - relevant auth info re mib module
*/

int
in_a_view(oid		  *name,      /* IN - name of var, OUT - name matched */
          size_t	  *namelen,   /* IN -number of sub-ids in name*/
          struct snmp_pdu *pdu,       /* IN - relevant auth info re PDU */
          int	           type)      /* IN - variable type being checked */
{

  struct view_parameters view_parms;
  view_parms.pdu = pdu;
  view_parms.name = name;
  if (namelen)
      view_parms.namelen = *namelen;
  else
      view_parms.namelen = 0;
  view_parms.errorcode = 0;

  if (pdu->flags & UCD_MSG_FLAG_ALWAYS_IN_VIEW)
    return VACM_SUCCESS; /* Enable bypassing of view-based access control */

  /* check for v1 and counter64s, since snmpv1 doesn't support it */
  if (pdu->version == SNMP_VERSION_1 && type == ASN_COUNTER64)
    return VACM_NOTINVIEW;
  switch (pdu->version) {
  case SNMP_VERSION_1:
  case SNMP_VERSION_2c:
  case SNMP_VERSION_3:
    snmp_call_callbacks(SNMP_CALLBACK_APPLICATION, SNMPD_CALLBACK_ACM_CHECK,
                        &view_parms);
    return view_parms.errorcode;
  }
  return VACM_NOSECNAME;
}

/* in_a_view: determines if a given snmp_pdu is ever going to be allowed to do
   anynthing or if it's not going to ever be authenticated. */
int
check_access(struct snmp_pdu *pdu)      /* IN - pdu being checked */
{
  struct view_parameters view_parms;
  view_parms.pdu = pdu;
  view_parms.name = 0;
  view_parms.namelen = 0;
  view_parms.errorcode = 0;

  if (pdu->flags & UCD_MSG_FLAG_ALWAYS_IN_VIEW)
    return 0;		/* Enable bypassing of view-based access control */

  switch (pdu->version) {
  case SNMP_VERSION_1:
  case SNMP_VERSION_2c:
  case SNMP_VERSION_3:
    snmp_call_callbacks(SNMP_CALLBACK_APPLICATION,
                        SNMPD_CALLBACK_ACM_CHECK_INITIAL,
                        &view_parms);
    return view_parms.errorcode;
  }
  return 1;
}

struct subtree *find_subtree_previous(oid *name,
                                      size_t len,
                                      struct subtree *subtree,
                                      const char *context_name)
{
  struct subtree *myptr, *previous = NULL;

  if ( subtree )
	myptr = subtree;
  else
	myptr = find_first_subtree(context_name);  /* look through everything */

  for( ; myptr != NULL; previous = myptr, myptr = myptr->next) {
    if (snmp_oid_compare(name, len, myptr->start, myptr->start_len) < 0) {
      return previous;
    }
  }
  return previous;
}

struct subtree *find_subtree_next(oid *name, 
				  size_t len,
				  struct subtree *subtree,
                                  const char *context_name)
{
  struct subtree *myptr = NULL;

  myptr = find_subtree_previous(name, len, subtree, context_name);

  if (myptr != NULL) {
    myptr = myptr->next;
    while (myptr && (myptr->variables == NULL || myptr->variables_len == 0)) {
      myptr = myptr->next;
    }
    return myptr;
  } else if (subtree &&
	 snmp_oid_compare(name, len, subtree->start, subtree->start_len) < 0) {
    return subtree;
  } else {
    return NULL;
  }
}

struct subtree *find_subtree(oid *name,
			     size_t len,
			     struct subtree *subtree,
                             const char *context_name)
{
  struct subtree *myptr;

  myptr = find_subtree_previous(name, len, subtree, context_name);
  if (myptr && snmp_oid_compare(name, len, myptr->end, myptr->end_len) < 0)
	return myptr;

  return NULL;
}

struct snmp_session *get_session_for_oid( oid *name, size_t len,
                                          const char *context_name)
{
   struct subtree *myptr;

   myptr = find_subtree_previous(name, len, find_first_subtree(context_name),
                                 context_name);
   while ( myptr && myptr->variables == NULL )
        myptr = myptr->next;

   if ( myptr == NULL )
        return NULL;
   else
        return myptr->session;
}



static struct subtree root_subtrees[] = {
   { { 0 }, 1 },	/* ccitt */
   { { 1 }, 1 },	/*  iso  */
   { { 2 }, 1 }		/* joint-ccitt-iso */
};


void setup_tree (void)
{
#ifdef USING_AGENTX_SUBAGENT_MODULE
  int role;

  role = ds_get_boolean(DS_APPLICATION_ID, DS_AGENT_ROLE);
  ds_set_boolean(DS_APPLICATION_ID, DS_AGENT_ROLE, MASTER_AGENT);
#endif

  register_null(root_subtrees[0].name,  root_subtrees[0].namelen);
  register_null(root_subtrees[1].name,  root_subtrees[1].namelen);
  register_null(root_subtrees[2].name,  root_subtrees[2].namelen);
  
  /* Support for 'static' subtrees (subtrees_old) has now been dropped */

  /* No longer necessary to sort the mib tree - this is inherent in
     the construction of the subtree structure */

#ifdef USING_AGENTX_SUBAGENT_MODULE
  ds_set_boolean(DS_APPLICATION_ID, DS_AGENT_ROLE, role);
#endif
}


extern void dump_idx_registry( void );
void dump_registry( void )
{
  struct variable *vp = NULL;
  struct subtree *myptr, *myptr2;
  u_char *s = NULL, *e = NULL, *v = NULL;
  size_t sl = 256, el = 256, vl = 256, sl_o = 0, el_o = 0, vl_o = 0;
  int i = 0;

  if ((s = (u_char *)calloc(sl, 1)) != NULL &&
      (e = (u_char *)calloc(sl, 1)) != NULL &&
      (v = (u_char *)calloc(sl, 1)) != NULL) {

    subtree_context_cache *ptr;
    for(ptr = context_subtrees; ptr; ptr = ptr->next) {
      printf("Subtrees for Context: %s\n", ptr->context_name);
      for(myptr = ptr->first_subtree ; myptr != NULL; myptr = myptr->next) {
	sl_o = el_o = vl_o = 0;

	if (!sprint_realloc_objid(&s, &sl, &sl_o, 1,
				  myptr->start, myptr->start_len)) {
	  break;
	}
	if (!sprint_realloc_objid(&e, &el, &el_o, 1,
				  myptr->end, myptr->end_len)) {
	  break;
	}
				  
	if (myptr->variables) {
	  printf("%02x ( %s - %s ) [", myptr->flags, s, e);
	  for (i = 0, vp = myptr->variables; i < myptr->variables_len; i++) {
	    vl_o = 0;
	    if (!sprint_realloc_objid(&v, &vl, &vl_o, 1, vp->name, vp->namelen)){
	      break;
	    }
	    printf("%s, ", v);
	    vp = (struct variable *)((char *)vp + myptr->variables_width);
	  }
	  printf("]\n");
	} else {
	  printf("%02x   %s - %s  \n", myptr->flags, s, e);
	}
	for(myptr2 = myptr; myptr2 != NULL; myptr2 = myptr2->children) {
	  if (myptr2->label && myptr2->label[0]) {
	    if (strcmp(myptr2->label, "old_api") == 0) {
	      struct variable *vp = myptr2->reginfo->handler->myvoid;
	      
	      sprint_realloc_objid(&s, &sl, &sl_o, 1, vp->name, vp->namelen);
	      printf("\t%s[%s] %p var %s\n", myptr2->label, 
		     myptr2->reginfo->handlerName?myptr2->reginfo->handlerName:"no-name", myptr2->reginfo, s);
	    } else {
	      printf("\t%s %s %p\n", myptr2->label, myptr2->reginfo->handlerName?myptr2->reginfo->handlerName:"no-handler-name", myptr2->reginfo);
	    }
	  }
	}
      }
    }
  }

  if (s != NULL) {
    free(s);
  }
  if (e != NULL) {
    free(e);
  }
  if (v != NULL) {
    free(v);
  }

  dump_idx_registry();
}


int external_readfd[NUM_EXTERNAL_FDS], external_readfdlen = 0;
int external_writefd[NUM_EXTERNAL_FDS], external_writefdlen = 0;
int external_exceptfd[NUM_EXTERNAL_FDS], external_exceptfdlen = 0;
void (* external_readfdfunc[NUM_EXTERNAL_FDS])(int, void *);
void (* external_writefdfunc[NUM_EXTERNAL_FDS])(int, void *);
void (* external_exceptfdfunc[NUM_EXTERNAL_FDS])(int, void *);
void *external_readfd_data[NUM_EXTERNAL_FDS];
void *external_writefd_data[NUM_EXTERNAL_FDS];
void *external_exceptfd_data[NUM_EXTERNAL_FDS];

int register_readfd(int fd, void (*func)(int, void *), void *data) {
    if (external_readfdlen < NUM_EXTERNAL_FDS) {
	external_readfd[external_readfdlen] = fd;
	external_readfdfunc[external_readfdlen] = func;
	external_readfd_data[external_readfdlen] = data;
	external_readfdlen++;
	DEBUGMSGTL(("register_readfd", "registered fd %d\n", fd));
	return FD_REGISTERED_OK;
    } else {
	snmp_log(LOG_CRIT, "register_readfd: too many file descriptors\n");
	return FD_REGISTRATION_FAILED;
    }
}

int register_writefd(int fd, void (*func)(int, void *), void *data) {
    if (external_writefdlen < NUM_EXTERNAL_FDS) {
	external_writefd[external_writefdlen] = fd;
	external_writefdfunc[external_writefdlen] = func;
	external_writefd_data[external_writefdlen] = data;
	external_writefdlen++;
	DEBUGMSGTL(("register_writefd", "registered fd %d\n", fd));
	return FD_REGISTERED_OK;
    } else {
	snmp_log(LOG_CRIT, "register_writefd: too many file descriptors\n");
	return FD_REGISTRATION_FAILED;
    }
}

int register_exceptfd(int fd, void (*func)(int, void *), void *data) {
    if (external_exceptfdlen < NUM_EXTERNAL_FDS) {
	external_exceptfd[external_exceptfdlen] = fd;
	external_exceptfdfunc[external_exceptfdlen] = func;
	external_exceptfd_data[external_exceptfdlen] = data;
	external_exceptfdlen++;
	DEBUGMSGTL(("register_exceptfd", "registered fd %d\n", fd));
	return FD_REGISTERED_OK;
    } else {
	snmp_log(LOG_CRIT, "register_exceptfd: too many file descriptors\n");
	return FD_REGISTRATION_FAILED;
    }
}

int unregister_readfd(int fd) {
    int i, j;

    for (i = 0; i < external_readfdlen; i++) {
	if (external_readfd[i] == fd) {
	    external_readfdlen--;
	    for (j = i; j < external_readfdlen; j++) {
		external_readfd[j] = external_readfd[j+1];
		external_readfd_data[j] = external_readfd_data[j+1];
	    }
	    DEBUGMSGTL(("unregister_readfd", "unregistered fd %d\n", fd));
	    return FD_UNREGISTERED_OK;
	}
    }
    return FD_NO_SUCH_REGISTRATION;
}

int unregister_writefd(int fd) {
    int i, j;

    for (i = 0; i < external_writefdlen; i++) {
	if (external_writefd[i] == fd) {
	    external_writefdlen--;
	    for (j = i; j < external_writefdlen; j++) {
		external_writefd[j] = external_writefd[j+1];
		external_writefd_data[j] = external_writefd_data[j+1];
	    }
	    DEBUGMSGTL(("unregister_writefd", "unregistered fd %d\n", fd));
	    return FD_UNREGISTERED_OK;
	}
    }
    return FD_NO_SUCH_REGISTRATION;
}

int unregister_exceptfd(int fd) {
    int i, j;

    for (i = 0; i < external_exceptfdlen; i++) {
	if (external_exceptfd[i] == fd) {
	    external_exceptfdlen--;
	    for (j = i; j < external_exceptfdlen; j++) {
		external_exceptfd[j] = external_exceptfd[j+1];
		external_exceptfd_data[j] = external_exceptfd_data[j+1];
	    }
	    DEBUGMSGTL(("unregister_exceptfd", "unregistered fd %d\n", fd));
	    return FD_UNREGISTERED_OK;
	}
    }
    return FD_NO_SUCH_REGISTRATION;
}

int external_signal_scheduled[NUM_EXTERNAL_SIGS];
void (* external_signal_handler[NUM_EXTERNAL_SIGS])(int);

#ifndef WIN32

/*
 * TODO: add agent_SIGXXX_handler functions and `case SIGXXX: ...' lines
 *       below for every single that might be handled by register_signal().
 */

RETSIGTYPE agent_SIGCHLD_handler(int sig)
{
  external_signal_scheduled[SIGCHLD]++;
#ifndef HAVE_SIGACTION
  /* signal() sucks. It *might* have SysV semantics, which means that
   * a signal handler is reset once it gets called. Ensure that it
   * remains active.
   */
  signal(SIGCHLD, agent_SIGCHLD_handler);
#endif
}

int register_signal(int sig, void (*func)(int))
{

    switch (sig) {
#if defined(SIGCHLD)
    case SIGCHLD:
#ifdef HAVE_SIGACTION
	{
		static struct sigaction act;
		act.sa_handler = agent_SIGCHLD_handler;
		sigemptyset(&act.sa_mask);
		act.sa_flags = 0;
		sigaction(SIGCHLD, &act, NULL);
	}
#else
	signal(SIGCHLD, agent_SIGCHLD_handler);
#endif
	break;
#endif
    default:
	snmp_log(LOG_CRIT,
		 "register_signal: signal %d cannot be handled\n", sig);
	return SIG_REGISTRATION_FAILED;
    }

    external_signal_handler[sig] = func;
    external_signal_scheduled[sig] = 0;
    
    DEBUGMSGTL(("register_signal", "registered signal %d\n", sig));
    return SIG_REGISTERED_OK;
}

int unregister_signal(int sig) {
    signal(sig, SIG_DFL);
    DEBUGMSGTL(("unregister_signal", "unregistered signal %d\n", sig));
    return SIG_UNREGISTERED_OK;
}

#endif /* !WIN32 */
