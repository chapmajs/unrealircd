/*
 * Channel Mode +j.
 * (C) Copyright 2005-.. The UnrealIRCd team.
 */

#include "config.h"
#include "struct.h"
#include "common.h"
#include "sys.h"
#include "numeric.h"
#include "msg.h"
#include "proto.h"
#include "channel.h"
#include <time.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#endif
#include <fcntl.h>
#include "h.h"
#ifdef _WIN32
#include "version.h"
#endif


ModuleHeader MOD_HEADER(jointhrottle)
  = {
	"chanmodes/jointhrottle",
	"$Id$",
	"Channel Mode +j",
	"3.2-b8-1",
	NULL,
    };

ModuleInfo *ModInfo = NULL;

ModDataInfo *jointhrottle_md; /* Module Data structure which we acquire */

Cmode_t EXTMODE_JOINTHROTTLE = 0L;

typedef struct {
	unsigned short num;
	unsigned short t;
} aModejEntry;

typedef struct JFlood aJFlood;

struct JFlood {
	aJFlood *prev, *next;
	char chname[CHANNELLEN+1];
	time_t firstjoin;
	unsigned short numjoins;
};

/* Forward declarations */
int cmodej_is_ok(aClient *sptr, aChannel *chptr, char mode, char *para, int type, int what);
void *cmodej_put_param(void *r_in, char *param);
char *cmodej_get_param(void *r_in);
char *cmodej_conv_param(char *param_in, aClient *sptr);
void cmodej_free_param(void *r);
void *cmodej_dup_struct(void *r_in);
int cmodej_sjoin_check(aChannel *chptr, void *ourx, void *theirx);

void jointhrottle_md_free(ModData *m);

int jointhrottle_can_join(aClient *sptr, aChannel *chptr, char *key, char *link, char *parv[]);
int jointhrottle_local_join(aClient *cptr, aClient *sptr, aChannel *chptr, char *parv[]);
static int isjthrottled(aClient *cptr, aChannel *chptr);
static void cmodej_increase_usercounter(aClient *cptr, aChannel *chptr);
EVENT(cmodej_cleanup_structs);
aJFlood *cmodej_addentry(aClient *cptr, aChannel *chptr);

DLLFUNC int MOD_INIT(jointhrottle)(ModuleInfo *modinfo)
{
	ModDataInfo mreq;
	CmodeInfo req;

	ModuleSetOptions(modinfo->handle, MOD_OPT_PERM_RELOADABLE, 1);
	MARK_AS_OFFICIAL_MODULE(modinfo);
	ModInfo = modinfo;

	memset(&req, 0, sizeof(req));
	req.paracount = 1;
	req.is_ok = cmodej_is_ok;
	req.flag = 'j';
	req.put_param = cmodej_put_param;
	req.get_param = cmodej_get_param;
	req.conv_param = cmodej_conv_param;
	req.free_param = cmodej_free_param;
	req.dup_struct = cmodej_dup_struct;
	req.sjoin_check = cmodej_sjoin_check;
	CmodeAdd(modinfo->handle, req, &EXTMODE_JOINTHROTTLE);

	memset(&mreq, 0, sizeof(mreq));
	mreq.name = "jointhrottle";
	mreq.free = jointhrottle_md_free;
	mreq.serialize = NULL; /* not supported */
	mreq.unserialize = NULL; /* not supported */
	mreq.sync = 0;
	mreq.type = MODDATATYPE_CLIENT;
	jointhrottle_md = ModDataAdd(modinfo->handle, mreq);
	if (!jointhrottle_md)
		abort();

	HookAddEx(modinfo->handle, HOOKTYPE_CAN_JOIN, jointhrottle_can_join);
	HookAddEx(modinfo->handle, HOOKTYPE_LOCAL_JOIN, jointhrottle_local_join);
	return MOD_SUCCESS;
}

DLLFUNC int MOD_LOAD(jointhrottle)(int module_load)
{
	EventAddEx(ModInfo->handle, "cmodej_cleanup_structs", 60, 0, cmodej_cleanup_structs, NULL);
	return MOD_SUCCESS;
}

DLLFUNC int MOD_UNLOAD(jointhrottle)(int module_unload)
{
	return MOD_FAILED;
}

int cmodej_is_ok(aClient *sptr, aChannel *chptr, char mode, char *para, int type, int what)
{
	if ((type == EXCHK_ACCESS) || (type == EXCHK_ACCESS_ERR))
	{
		if (IsPerson(sptr) && is_chan_op(sptr, chptr))
			return EX_ALLOW;
		if (type == EXCHK_ACCESS_ERR) /* can only be due to being halfop */
			sendto_one(sptr, err_str(ERR_NOTFORHALFOPS), me.name, sptr->name, 'j');
		return EX_DENY;
	} else
	if (type == EXCHK_PARAM)
	{
		/* Check parameter.. syntax should be X:Y, X should be 1-255, Y should be 1-999 */
		char buf[32], *p;
		int num, t, fail = 0;
		
		strlcpy(buf, para, sizeof(buf));
		p = strchr(buf, ':');
		if (!p)
		{
			fail = 1;
		} else {
			*p++ = '\0';
			num = atoi(buf);
			t = atoi(p);
			if ((num < 1) || (num > 255) || (t < 1) || (t > 999))
				fail = 1;
		}
		if (fail)
		{
			sendnotice(sptr, "Error in setting +j, syntax: +j <num>:<seconds>, where <num> must be 1-255, and <seconds> 1-999");
			return EX_DENY;
		}
		return EX_ALLOW;
	}

	/* falltrough -- should not be used */
	return EX_DENY;
}

void *cmodej_put_param(void *r_in, char *param)
{
aModejEntry *r = (aModejEntry *)r_in;
char buf[32], *p;
int num, t;

	if (!r)
	{
		/* Need to create one */
		r = (aModejEntry *)MyMallocEx(sizeof(aModejEntry));
	}
	strlcpy(buf, param, sizeof(buf));
	p = strchr(buf, ':');
	if (p)
	{
		*p++ = '\0';
		num = atoi(buf);
		t = atoi(p);
		if (num < 1) num = 1;
		if (num > 255) num = 255;
		if (t < 1) t = 1;
		if (t > 999) t = 999;
		r->num = num;
		r->t = t;
	} else {
		r->num = 0;
		r->t = 0;
	}
	return (void *)r;
}

char *cmodej_get_param(void *r_in)
{
aModejEntry *r = (aModejEntry *)r_in;
static char retbuf[16];

	if (!r)
		return NULL;

	snprintf(retbuf, sizeof(retbuf), "%hu:%hu", r->num, r->t);
	return retbuf;
}

char *cmodej_conv_param(char *param_in, aClient *sptr)
{
static char retbuf[32];
char param[32], *p;
int num, t, fail = 0;
		
	strlcpy(param, param_in, sizeof(param));
	p = strchr(param, ':');
	if (!p)
		return NULL;
	*p++ = '\0';
	num = atoi(param);
	t = atoi(p);
	if (num < 1)
		num = 1;
	if (num > 255)
		num = 255;
	if (t < 1)
		t = 1;
	if (t > 999)
		t = 999;
	
	snprintf(retbuf, sizeof(retbuf), "%d:%d", num, t);
	return retbuf;
}

void cmodej_free_param(void *r)
{
	MyFree(r);
}

void *cmodej_dup_struct(void *r_in)
{
aModejEntry *r = (aModejEntry *)r_in;
aModejEntry *w = (aModejEntry *)MyMalloc(sizeof(aModejEntry));

	memcpy(w, r, sizeof(aModejEntry));
	return (void *)w;
}

int cmodej_sjoin_check(aChannel *chptr, void *ourx, void *theirx)
{
aModejEntry *our = (aModejEntry *)ourx;
aModejEntry *their = (aModejEntry *)theirx;

	if (our->t != their->t)
	{
		if (our->t > their->t)
			return EXSJ_WEWON;
		else
			return EXSJ_THEYWON;
	}
	else if (our->num != their->num)
	{
		if (our->num > their->num)
			return EXSJ_WEWON;
		else
			return EXSJ_THEYWON;
	} else
		return EXSJ_SAME;
}

static int isjthrottled(aClient *cptr, aChannel *chptr)
{
aModejEntry *m;
aJFlood *e;
int num=0, t=0;

	if (!MyClient(cptr))
		return 0;

	m = (aModejEntry *)GETPARASTRUCT(chptr, 'j');
	if (!m) return 0; /* caller made an error? */
	num = m->num;
	t = m->t;

#ifdef DEBUGMODE
	if (!num || !t)
		abort();
#endif

	/* Grab user<->chan entry.. */
	for (e = moddata_client(cptr, jointhrottle_md).ptr; e; e=e->next)
		if (!strcasecmp(e->chname, chptr->chname))
			break;
	
	if (!e)
		return 0; /* Not present, so cannot be throttled */

	/* Ok... now the actual check:
	 * if ([timer valid] && [one more join would exceed num])
	 */
	if (((TStime() - e->firstjoin) < t) && (e->numjoins == num))
		return 1; /* Throttled */

	return 0;
}

static void cmodej_increase_usercounter(aClient *cptr, aChannel *chptr)
{
aModejEntry *m;
aJFlood *e;
int num=0, t=0;

	if (!MyClient(cptr))
		return;
		
	m = (aModejEntry *)GETPARASTRUCT(chptr, 'j');
	if (!m) return;
	num = m->num;
	t = m->t;

#ifdef DEBUGMODE
	if (!num || !t)
		abort();
#endif

	/* Grab user<->chan entry.. */
	for (e = moddata_client(cptr, jointhrottle_md).ptr; e; e=e->next)
		if (!strcasecmp(e->chname, chptr->chname))
			break;
	
	if (!e)
	{
		/* Allocate one */
		e = cmodej_addentry(cptr, chptr);
		e->firstjoin = TStime();
		e->numjoins = 1;
	} else
	if ((TStime() - e->firstjoin) < t) /* still valid? */
	{
		e->numjoins++;
	} else {
		/* reset :p */
		e->firstjoin = TStime();
		e->numjoins = 1;
	}
}

int jointhrottle_can_join(aClient *sptr, aChannel *chptr, char *key, char *link, char *parv[])
{
	if (!IsAnOper(sptr) &&
	    (chptr->mode.extmode & EXTMODE_JOINTHROTTLE) && isjthrottled(sptr, chptr))
		return ERR_TOOMANYJOINS;
	return 0;
}


int jointhrottle_local_join(aClient *cptr, aClient *sptr, aChannel *chptr, char *parv[])
{
	cmodej_increase_usercounter(cptr, chptr);
	return 0;
}

/** Adds a aJFlood entry to user & channel and returns entry.
 * NOTE: Does not check for already-existing-entry
 */
aJFlood *cmodej_addentry(aClient *cptr, aChannel *chptr)
{
aJFlood *e;

#ifdef DEBUGMODE
	if (!IsPerson(cptr))
		abort();

	for (e=moddata_client(cptr, jointhrottle_md).ptr; e; e=e->next)
		if (!strcasecmp(e->chname, chptr->chname))
			abort(); /* already exists -- should never happen */
#endif

	e = MyMallocEx(sizeof(aJFlood));
	strlcpy(e->chname, chptr->chname, sizeof(e->chname));

	/* Insert our new entry as (new) head */
	if (moddata_client(cptr, jointhrottle_md).ptr)
	{
		aJFlood *current_head = moddata_client(cptr, jointhrottle_md).ptr;
		current_head->prev = e;
		e->next = current_head;
	}
	moddata_client(cptr, jointhrottle_md).ptr = e;

	return e;
}

/** Regularly cleans up cmode-j user/chan structs */
EVENT(cmodej_cleanup_structs)
{
aClient *acptr;
aChannel *chptr;
aJFlood *jf, *jf_next;
int t;
	
	list_for_each_entry(acptr, &lclient_list, lclient_node)
	{
		if (!MyClient(acptr))
			continue; /* only (local) persons.. */

		for (jf = moddata_client(acptr, jointhrottle_md).ptr; jf; jf = jf_next)
		{
			t = 0;
			jf_next = jf->next;
			
			chptr = find_channel(jf->chname, NULL);
			/* Now check if chptr is valid and if a flood still applies, if so we skip,
			 * in all other cases we free the (no longer useful) entry.
			 */
			if (chptr && (chptr->mode.extmode & EXTMODE_JOINTHROTTLE))
			{
				/* exists and has +j set.. check some more..*/
				t = ((aModejEntry *)GETPARASTRUCT(chptr, 'j'))->t;

				if (jf->firstjoin + t > TStime())
					continue; /* still valid entry */
			}
#ifdef DEBUGMODE
			ircd_log(LOG_ERROR, "cmodej_cleanup_structs(): freeing %s/%s (%ld[%ld], %d)",
				acptr->name, jf->chname, jf->firstjoin, (long)(TStime() - jf->firstjoin), t);
#endif
			if (moddata_client(acptr, jointhrottle_md).ptr == jf)
			{
				/* change head */
				moddata_client(acptr, jointhrottle_md).ptr = jf->next; /* could be set to NULL now */
				if (jf->next)
					jf->next->prev = NULL;
			} else {
				/* change non-head entries */
				jf->prev->next = jf->next; /* could be set to NULL now */
				if (jf->next)
					jf->next->prev = jf->prev;
			}
			MyFree(jf);
		}
	}
}

void jointhrottle_md_free(ModData *m)
{
aJFlood *j, *j_next;

	if (!m->ptr)
		return;

	for (j = m->ptr; j; j = j_next)
	{
		j_next = j->next;
		MyFree(j);
	}	

	m->ptr = NULL;
}
