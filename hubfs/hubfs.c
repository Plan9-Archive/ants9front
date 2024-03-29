#include <u.h>
#include <libc.h>
#include <auth.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include <ctype.h>
#include "ratelimit.h"

/* input/output multiplexing and buferring */
/* often used in combination with hubshell client and hub wrapper script */

/* Flags track the state of queued 9p requests and ketchup/wait in paranoid mode */
enum flags{
	DOWN = 0,
	UP = 1,
	WAIT = 2,
	DONE = 3,
};

enum buffersizes{
	MAGIC = 77777,				/* In paranoid mode let readers lag this many bytes */
	MAXQ = 777,					/* Maximum number of 9p requests to queue */
	SMBUF = 777,				/* Buffer for names and other small strings */
	MAXHUBS = 77,				/* Total number of hubs that can be created */
};

typedef struct Hub	Hub;		/* A Hub file is a multiplexed pipe-like data buffer */
typedef struct Msgq	Msgq;		/* Client fid structure to track location */
typedef struct Hublist Hublist;	/* Linked list of hubs */

struct Hub{
	char name[SMBUF];			/* name */
	char *bucket;				/* pointer to data buffer */
	char *inbuckp;				/* location to store next message */
	int buckfull;				/* amount of data stored in bucket */
	char *buckwrap;				/* exact limit of written data before pointer reset */
	Req *qreads[MAXQ];			/* pointers to queued read Reqs */
	int rstatus[MAXQ];			/* status of read requests */
	int qrnum;					/* index of read Reqs waiting to be filled */
	int qrans;					/* number of read Reqs answered */
	Req *qwrites[MAXQ];			/* Similar for write Reqs */
	int wstatus[MAXQ];
	int qwnum;
	int qwans;
	int ketchup;				/* lag of readers vs. writers in paranoid mode */
	int tomatoflag;				/* readers use tomatoflag to tell writers to wait */
	QLock wrlk;					/* writer lock during fear */
	QLock replk;				/* reply lock during fear */
	int killme;					/* forked processes in paranoid mode need to exit */
	Limiter *lp;				/* Pointer to limiter struct for this hub */
	vlong bp;					/* Bytes per second that can be written */
	vlong st;					/* minimum separation time between messages in ns */
	vlong rt;					/* Interval in seconds for resetting limit timer */
};

struct Msgq{
	ulong myfid;				/* Msgq is associated with client fids */
	char *nxt;					/* Location of this client in the buffer */
	int bufuse;					/* how much of the buffer has been used */
};

struct Hublist{
	Hub *targethub;				/* Hub referenced by this entry */
	char *hubname;				/* Name of referenced Hub */
	Hublist *nexthub;			/* Pointer to next Hublist entry in list */
};

Hublist *firsthublist;			/* Pointer to start of linked list of hubs */
Hublist *lasthublist;			/* Pointer to list entry for next hub to be created */
char *srvname;					/* Name of this hubfs service */
int numhubs;					/* Total number of hubs in existence */
int paranoia;					/* Paranoid mode maintains loose reader/writer sync */
int freeze;						/* In frozen mode the hubs operate simply as a ramfs */
int trunc;						/* In trunc mode only new data is sent, not buffered */
int allowzap;					/* Determine whether a buffer can be emptied forcibly */
int endoffile;					/* Send zero length end of file read to all clients */
int applylimits;				/* Whether time/rate limits are applied */
vlong bytespersecond;			/* Bytes per second allowed by rate limiting */
vlong separationinterval;		/* Minimum time between writes in nanoseconds */
vlong resettime;				/* Number of seconds between writes ratelimit reset */
u32int maxmsglen;				/* Maximum message length accepted */
uvlong bucksize;					/* Size of data bucket per hub */

static char Ebad[] = "something bad happened";
static char Enomem[] = "no memory";

void wrsend(Hub *h);
void msgsend(Hub *h);
void hubcmd(char *cmd);
void setuphub(Hub *h);
void addhub(Hub *h);
void removehub(Hub *h);
void eofall(void);
void eofhub(char *target);
int flushcheck(Hub *h, Req *r);

void fsread(Req *r);
void fswrite(Req *r);
void fscreate(Req *r);
void fsopen(Req *r);
void fsflush(Req *r);
void fsdestroyfile(File *f);
void usage(void);

Srv fs = {
	.open = fsopen,
	.read = fsread,
	.write = fswrite,
	.create = fscreate,
	.flush = fsflush,
};

/*
 * Rate limiting is only applied if specified by flags.
 * The limiting parameters are global for the hubfs.
 * Each hubfile tracks its own limits separately.
*/

/*
 * Basic logic - we have a buffer/bucket of data (a hub) that is mapped to a file.
 * For each hub we keep two queues of 9p requests, one for reads and one for writes.
 * As requests come in, we add them to the queue, then fill waiting queued requests.
 * The data buffers are statically sized at creation. Data is continuously read
 * and written in a rotating pattern. At the end, we wrap back around to the start. 
 * Our job is accurately transferring the bytes in and out of the bucket and 
 * tracking the location of the read and write pointers for each writer and reader. 
*/

/* msgsend replies to Reqs queued by fsread */
void
msgsend(Hub *h)
{
	Req *r;
	Msgq *mq;
	u32int count;
	int i;

	if(h->qrnum == 0)
		return;

	/* loop through queued 9p read requests for this hub and answer if needed */
	for(i = h->qrans; i <= h->qrnum; i++){
		if(paranoia == UP)
			qlock(&h->replk);
		if(h->rstatus[i] != WAIT){
			if((i == h->qrans) && (i < h->qrnum))
				h->qrans++;
			if(paranoia == UP)
				qunlock(&h->replk);
			continue;
		}

		/* request found, if it has read all data keep it waiting unless eof sent */
		r = h->qreads[i];
		mq = r->fid->aux;
		if(mq->nxt == h->inbuckp){
			if(paranoia == UP)
				qunlock(&h->replk);
			if(endoffile == UP){
				r->ofcall.count = 0;
				h->rstatus[i] = DONE;
				if((i == h->qrans) && (i < h->qrnum))
					h->qrans++;
				respond(r, nil);
				continue;
			}
			continue;
		}
		count = r->ifcall.count;

		/* if the mq has read to the wrap point, send it back to start */
		if(mq->nxt >= h->buckwrap){
			mq->nxt = h->bucket;
			mq->bufuse = 0;
		}
		/* if the mq next read would go past wrap point, read up to wrap */
		if(mq->nxt + count > h->buckwrap)
			count = h->buckwrap - mq->nxt;
		/* if reader asks for more data than remains in bucket, adjust down */
		if((mq->bufuse + count > h->buckfull) && (mq->bufuse < h->buckfull))
			count = h->buckfull - mq->bufuse;

		/* Done with reader location and count checks, now we can send the data */
		memmove(r->ofcall.data, mq->nxt, count);
		r->ofcall.count = count;
		mq->nxt += count;
		mq->bufuse += count;
		h->rstatus[i] = DONE;
		if((i == h->qrans) && (i < h->qrnum))
			h->qrans++;
		respond(r, nil);

		if(paranoia == UP){
			h->ketchup = mq->bufuse;
			if(mq->bufuse <= h->buckfull)
				h->tomatoflag = DOWN;	/* DOWN means do not wait for us */
			else
				h->tomatoflag = UP;
			qunlock(&h->replk);
		}
	}
}

/* wrsend replies to Reqs queued by fswrite */
void
wrsend(Hub *h)
{
	Req *r;
	u32int count;
	int i;
	int j;

	if(h->qwnum == 0)
		return;

	/* in paranoid mode we fork and slack off while the readers catch up */
	if(paranoia == UP){
		qlock(&h->wrlk);
		if((h->ketchup < h->buckfull - MAGIC) || (h->ketchup > h->buckfull)){
			if(rfork(RFPROC|RFMEM) == 0){
				sleep(100);
				h->killme = UP;
				for(j = 0; ((j < 77) && (h->tomatoflag == UP)); j++)
					sleep(7);		/* Give readers time to catch up */
			} else
				return;	/* This branch should become a read request */
		}
	}

	/* loop through queued 9p write requests for this hub */
	for(i = h->qwans; i <= h->qwnum; i++){
		if(h->wstatus[i] != WAIT){
			if((i == h->qwans) && (i < h->qwnum))
				h->qwans++;
			continue;
		}
		r = h->qwrites[i];
		count = r->ifcall.count;
		if(count > maxmsglen)
			count = maxmsglen;

		/* bucket wraparound check */
		if((h->buckfull + count) >= bucksize - 16){
			h->buckwrap = h->inbuckp;
			h->inbuckp = h->bucket;
			h->buckfull = 0;
		}

		/* Move the data into the bucket, update our counters, and respond */
		memmove(h->inbuckp, r->ifcall.data, count);
		h->inbuckp += count;
		if(h->inbuckp > h->buckwrap)
			h->buckwrap=h->inbuckp+1;
		h->buckfull += count;
		r->fid->file->length = h->buckfull;
		r->ofcall.count = count;
		h->wstatus[i] = DONE;
		if((i == h->qwans) && (i < h->qwnum))
			h->qwans++;
		if(applylimits)
			limit(h->lp, count);
		respond(r, nil);

		if(paranoia == UP){
			if(h->wrlk.locked == 1)
				qunlock(&h->wrlk);
			/* If killme is up we forked another flow of control, so exit */
			if(h->killme == UP){
				h->killme = DOWN;
				exits(nil);
			}
		}
	}
}

/* queue all reads unless Hubs are set to freeze */
void
fsread(Req *r)
{
	Hub *h;
	Msgq *mq;
	u32int count;
	vlong offset;
	char tmpstr[2*SMBUF];
	int i;
	int j;

	h = r->fid->file->aux;

	if(strncmp(h->name, "ctl", 3) == 0){
		count = r->ifcall.count;
		offset = r->ifcall.offset;
		if(offset > 0){
			r->ofcall.count = 0;
			respond(r, nil);
			return;
		}
		snprint(tmpstr, 2*SMBUF-1, "\tHubfs %s status (1 is active, 0 is inactive):\n \
Paranoia == %d  Freeze == %d  Trunc == %d  Applylimits == %d\n \
Buffersize == %ulld \n", srvname, paranoia, freeze, trunc, applylimits, bucksize);
		if(strlen(tmpstr) <= count)
			count = strlen(tmpstr);
		else
			respond(r, "read count too small to answer");
		memmove(r->ofcall.data, tmpstr, count);
		r->ofcall.count = count;
		respond(r, nil);
		return;
	}

	/* In freeze mode hubs behave as ramdisk files */
	if(freeze == UP){
		mq = r->fid->aux;
		if(mq->bufuse > 0){
			if(h->qrnum >= MAXQ - 2){
				j = 1;
				for(i = h->qrans; i <= h->qrnum; i++) {
					h->qreads[j] = h->qreads[i];
					h->rstatus[j] = h->rstatus[i];
					j++;
				}
				h->qrnum = h->qrnum - h->qrans + 1;
				h->qrans = 1;
			}
			h->qrnum++;
			h->rstatus[h->qrnum] = WAIT;
			h->qreads[h->qrnum] = r;
			return;
		}
		count = r->ifcall.count;
		offset = r->ifcall.offset;
		while(offset >= bucksize)
			offset -= bucksize;
		if(offset >= h->buckfull){
			r->ofcall.count = 0;
			respond(r, nil);
			return;
		}
		if((offset + count >= h->buckfull) && (offset < h->buckfull))
			count = h->buckfull - offset;
		memmove(r->ofcall.data, h->bucket + offset, count);
		r->ofcall.count = count;
		respond(r, nil);
		return;
	}

	/* Actual queue logic, ctl file and freeze mode logic is rarely used */
	if(h->qrnum >= MAXQ - 2){
		j = 1;
		for(i = h->qrans; i <= h->qrnum; i++) {
			h->qreads[j] = h->qreads[i];
			h->rstatus[j] = h->rstatus[i];
			j++;
		}
		h->qrnum = h->qrnum - h->qrans + 1;
		h->qrans = 1;
	}
	h->qrnum++;
	h->rstatus[h->qrnum] = WAIT;
	h->qreads[h->qrnum] = r;
	msgsend(h);
}

/* queue writes unless hubs are set to frozen mode */
void
fswrite(Req *r)
{
	Hub *h;
	u32int count;
	vlong offset;
	int i;
	int j;

	h = r->fid->file->aux;
	if(strncmp(h->name, "ctl", 3) == 0)
		hubcmd(r->ifcall.data);

	if(freeze == UP){
		count = r->ifcall.count;
		offset = r->ifcall.offset;
		while(offset >= bucksize)
			offset -= bucksize;
		h->inbuckp = h->bucket +offset;
		h->buckfull = h->inbuckp - h->bucket;
		if(h->buckfull + count >= bucksize){
			h->inbuckp = h->bucket;
			h->buckfull = 0;
		}
		memmove(h->inbuckp, r->ifcall.data, count);
		h->inbuckp += count;
		h->buckfull += count;
		r->fid->file->length = h->buckfull;
		r->ofcall.count = count;
		respond(r, nil);
		return;
	}

	/* Actual queue logic here */
	if(h->qwnum >= MAXQ - 2){
		j = 1;
		for(i = h->qwans; i <= h->qwnum; i++) {
			h->qwrites[j] = h->qwrites[i];
			h->wstatus[j] = h->wstatus[i];
			j++;
		}
		h->qwnum = h->qwnum - h->qwans + 1;
		h->qwans = 1;
	}
	h->qwnum++;
	h->wstatus[h->qwnum] = WAIT;
	h->qwrites[h->qwnum] = r;
	wrsend(h);
	msgsend(h);
	/* we do msgsend here after wrsend because we know a write has happened */
	/* that means there is new data for readers, so send it to them asap */
}

/* making a file is making a new hub, prepare it for i/o and add to hublist */
void
fscreate(Req *r)
{
	Hub *h;
	File *f;

	if(numhubs > MAXHUBS){
		fprint(2, "Too many hubs created\n");
		respond(r, Ebad);
		return;
	}
	if(f = createfile(r->fid->file, r->ifcall.name, r->fid->uid, r->ifcall.perm, nil)){
		numhubs++;
		h = (Hub*)emalloc9p(sizeof(Hub));
		setuphub(h);
		addhub(h);
		strncat(h->name,r->ifcall.name,SMBUF-1);
		f->aux = h;
		r->fid->file = f;
		r->ofcall.qid = f->qid;
		respond(r, nil);
		return;
	}
	respond(r, Ebad);
}

/* new client for the hubfile, create new message queue with client fid */
void
fsopen(Req *r)
{
	Hub *h;
	Msgq *q;

	h = r->fid->file->aux;
	if(!h){
		respond(r, nil);
		return;
	}
	q = (Msgq*)emalloc9p(sizeof(Msgq));

	q->myfid = r->fid->fid;
	q->nxt = h->bucket;
	q->bufuse = 0;
	if(r->ifcall.mode&OTRUNC){
		if(allowzap){
			h->inbuckp = h->bucket;
			h->buckfull = 0;
			r->fid->file->length = 0;
		}
	}
	if (trunc == UP){
		q->nxt = h->inbuckp;
		q->bufuse = h->buckfull;
	}
	r->fid->aux = q;
	respond(r, nil);
}

/* flush a pending request if the client asks us to */
void
fsflush(Req *r)
{
	Hublist *currenthub;
	int flushed;

	currenthub = firsthublist;
	flushed = flushcheck(currenthub->targethub, r);
	if(flushed)
		return;
	while(currenthub->nexthub->targethub != nil){
		currenthub=currenthub->nexthub;
		flushed = flushcheck(currenthub->targethub, r);
		if(flushed)
			return;
	}
	respond(r, nil);
}

/* check a hub to see if it contains a pending Req with matching tag */
int
flushcheck(Hub *h, Req *r)
{
	Req *tr;
	int i;

	for(i = h->qrans; i <= h->qrnum; i++){
		if(h->rstatus[i] != WAIT)
			continue;
		tr=h->qreads[i];
		if(tr->tag == r->ifcall.oldtag){
			tr->ofcall.count = 0;
			h->rstatus[i] = DONE;
			if((i == h->qrans) && (i < h->qrnum))
				h->qrans++;
			respond(tr, nil);
			respond(r, nil);
			return 1;
		}
	}
	for(i = h->qwans; i <= h->qwnum; i++){
		if(h->wstatus[i] != WAIT)
			continue;
		tr=h->qwrites[i];
		if(tr->tag == r->ifcall.oldtag){
			tr->ofcall.count = 0;
			h->wstatus[i] = DONE;
			if((i == h->qwans) && (i < h->qwnum))
				h->qwans++;
			respond(tr, nil);
			respond(r, nil);
			return 1;
		}
	}
	return 0;
}

/* delete the hub. We don't track the associated mqs of clients so we leak them. */
void
fsdestroyfile(File *f)
{
	Hub *h;

	numhubs--;
	h = f->aux;
	if(h){
		removehub(h);
		if(applylimits)
			free(h->lp);
		free(h->bucket);
		free(h);
	}
}

/* called when a hubfile is created */
/* ?Why is qrans set to 1 and qwans to 0 when both are set to 1 upon looping? */
void
setuphub(Hub *h)
{
	h->bucket = (char*)emalloc9p(bucksize);
	h->inbuckp = h->bucket;
	h->qrnum = 0;
	h->qrans = 1;
	h->qwnum = 0;
	h->qwans = 0;
	h->ketchup = 0;
	h->buckfull = 0;
	h->buckwrap = h->inbuckp + bucksize;
	if(applylimits){
		h->bp = bytespersecond;
		h->st = separationinterval;
		h->rt = resettime;
		h->lp = startlimit(SECOND/h->bp, h->st, h->rt * SECOND);
	}
}

/* add a new hub to the linked list of hubs */
void
addhub(Hub *h)
{
	lasthublist->targethub = h;
	lasthublist->hubname = h->name;
	lasthublist->nexthub = (Hublist*)emalloc9p(sizeof(Hublist)); /* keep an empty */
	lasthublist = lasthublist->nexthub;
	lasthublist->nexthub = nil;
	lasthublist->targethub = nil;
}

/* remove a hub about to be deleted from the linked list of hubs */
void
removehub(Hub *h)
{
	Hublist* currenthub;

	currenthub = firsthublist;
	if(currenthub->targethub = h){
		if(currenthub->nexthub != nil)
			firsthublist = currenthub->nexthub;
		free(currenthub);
		return;
	}
	while(currenthub->nexthub->targethub != nil){
		if(currenthub->nexthub->targethub = h){
			currenthub->nexthub = currenthub->nexthub->nexthub;
			free(currenthub->nexthub);
			return;
		}
		currenthub=currenthub->nexthub;
	}
}

/* issue eofs or set status of paranoid mode and frozen/normal from ctl messages */
void
hubcmd(char *cmd)
{
	int i;
	char cmdbuf[256];

	if(strncmp(cmd, "quit", 4) == 0)
		sysfatal("hubfs: quit command received");
	if(strncmp(cmd, "fear", 4) == 0){
		paranoia = UP;
		fprint(2, "hubfs: paranoid mode activated\n");
		return;
	}
	if(strncmp(cmd, "calm", 4) == 0){
		paranoia = DOWN;
		fprint(2, "hubfs: non-paranoid mode resumed\n");
		return;
	}
	if(strncmp(cmd, "freeze", 6) == 0){
		freeze = UP;
		fprint(2, "hubfs: the pipes freeze in place\n");
		return;
	}
	if(strncmp(cmd, "melt", 4) == 0){
		freeze = DOWN;
		fprint(2, "hubfs: the pipes thaw\n");
		return;
	}
	if(strncmp(cmd, "trunc", 5) == 0){
		trunc = UP;
		fprint(2, "trunc mode set\n");
		return;
	}
	if(strncmp(cmd, "notrunc", 7) == 0){
		trunc = DOWN;
		fprint(2, "trunc mode off\n");
		return;
	}

	/* eof command received, check if it applies to single hub then dispatch */
	if(strncmp(cmd, "eof", 3) == 0){
		endoffile = UP;
		if(strlen(cmd) > 4){
			i=0;
			while(isalnum(*(cmd+i+4))){
				cmdbuf[i]=*(cmd+i+4);
				i++;
			}
			cmdbuf[i] = '\0';
			eofhub(cmdbuf);
			endoffile = DOWN;
			return;
		}
		fprint(2, "hubfs: sending end of file to all client readers\n");
		eofall();
		endoffile = DOWN;
		return;
	}
	fprint(2, "hubfs: no matching command found\n");
}

/* send eof to specific named hub */
void
eofhub(char *target){
	Hublist *currenthub;

	currenthub = firsthublist;
	if(currenthub->targethub == nil)
		return;
	if(strcmp(target, currenthub->hubname) == 0){
		fprint(2, "hubfs: eof to %s\n", currenthub->hubname);
		msgsend(currenthub->targethub);
		return;
	}
	while(currenthub->nexthub->targethub != nil){
		currenthub=currenthub->nexthub;
		if(strcmp(target, currenthub->hubname) == 0){
			fprint(2, "hubfs: eof to %s\n", currenthub->hubname);
			msgsend(currenthub->targethub);
			return;
		}
	}
}

/* send eof to all hub readers */
void
eofall(){
	Hublist *currenthub;

	currenthub = firsthublist;
	if(currenthub->targethub == nil)
		return;
	msgsend(currenthub->targethub);
	while(currenthub->nexthub->targethub != nil){
		currenthub=currenthub->nexthub;
		msgsend(currenthub->targethub);
	}
}

void
usage(void)
{
	fprint(2, "usage: hubfs [-D] [-t] [-q bucketsize] [-b bytespersec] \
[-i nsbetweenmsgs] [-r timerreset] [-l maxmsglen] [-s srvname] [-m mtpt]\n");
	exits("usage");
}

void
main(int argc, char **argv)
{
	char *addr = nil;
	char *mtpt = nil;
	char *bps, *rst, *len, *spi, *qua;
	Qid q;
	srvname = nil;
	paranoia = DOWN;
	freeze = DOWN;
	trunc = DOWN;
	allowzap = DOWN;
	endoffile = DOWN;
	applylimits = DOWN;
	numhubs = 0;
	bytespersecond = 1073741824;
	separationinterval = 1;
	resettime =  60;
	maxmsglen = 666666;
	bucksize = 777777;

	fs.tree = alloctree(nil, nil, DMDIR|0777, fsdestroyfile);
	q = fs.tree->root->qid;

	ARGBEGIN{
	case 'D':
		chatty9p++;
		break;
	case 'q':
		qua = ARGF();
		if (qua == nil)
			usage();
		bucksize = strtoull(qua, 0 , 10);
		break;
	case 'b':
		bps = ARGF();
		if (bps == nil)
			usage();
		bytespersecond = strtoull(bps, 0, 10);
		applylimits = UP;
		break;
	case 'i':
		spi = ARGF();
		if (spi == nil)
			usage();
		separationinterval = strtoull(spi, 0, 10);
		applylimits = UP;
		break;
	case 'r':
		rst = ARGF();
		if (rst == nil)
			usage();
		resettime = strtoull(rst, 0, 10);
		applylimits = UP;
		break;
	case 'l':
		len = ARGF();
		if (len == nil)
			usage();
		maxmsglen = atoi(len);
		break;
	case 'a':
		addr = EARGF(usage());
		break;
	case 's':
		srvname = EARGF(usage());
		break;
	case 'm':
		mtpt = EARGF(usage());
		break;
	case 't':
		trunc = UP;
		break;
	case 'z':
		allowzap = UP;
		break;
	default:
		usage();
	}ARGEND;

	if(argc)
		usage();
	if(chatty9p)
		fprint(2, "hubsrv.nopipe %d srvname %s mtpt %s\n", fs.nopipe, srvname, mtpt);
	if(addr == nil && srvname == nil && mtpt == nil)
		sysfatal("must specify -a, -s, or -m option");

	/* start with an allocated but empty first Hublist entry */
	lasthublist = (Hublist*)emalloc9p(sizeof(Hublist));
	lasthublist->targethub = nil;
	lasthublist->hubname = nil;
	lasthublist->nexthub = nil;
	firsthublist = lasthublist;

	if(addr)
		listensrv(&fs, addr);
	if(srvname || mtpt)
		postmountsrv(&fs, srvname, mtpt, MREPL|MCREATE);
	exits(0);
}

/* basic 9pfile implementation taken from /sys/src/lib9p/ramfs.c */

/* A note on paranoid mode and writer/reader synchronization.  The
default behavior is "broadcast like" rather than pipelike, because
writers are never blocked by default.  This is because in a network
situation with remote readers, blocking writes to wait for remote
client reads to complete produces an unpleasant user experience where
the remote latency limits the local environment.  As a consequence, by
default it is up to the writer to a file to limit the quantity and
speed of data writen to what clients are able receive.  The normal
intended mode of operation is for shells, and in this case data is
both 'bursty' and usually fairly small.  */

/* Paranoid mode is intended as "safe mode" and changes this
first-come first served behavior.  In paranoid mode, the readers and
writers attempt to synchronize.  The hub ketchup and tomatoflag
variables are used to monitor if readers have 'fallen behind' the
current data pointer, and if so, the writer is qlocked and sleeps
while we fork off a new flow of control.  We need to do more than just
answer the queued reads - because we are inside the 9p library (we are
the functions called by its srv pointer) we want the 9p library to
actually answer the incoming reads so we have read messages queued to
answer.  Just clearing out the read message queue isn't enough to
prioritize readers - we need to fork off so the controlling 9p library
has a chance to answer NEW reads.  By forking and sleeping the writer,
we allow the os to answer a new read request, which will unlock the
writer, which then needs to die at the end of its function because we
are a single-threaded 9p server and need to maintain one master flow
of control.  */

/* The paranoid/safe mode code is still limited in its logic for
handling multiple readers.  The ketchup and tomatoflag are per hub,
and a hub can have multiple clients.  It is intentional that these
multiple clients will 'race for the flag' and the writer will stop
waiting when one reader catches up enough to set ketchup and tomato
flag.  A more comprehensive solution would require adding new
structure to the client mq and a ref-counting implementation for
attaches to the hub so the hub could check the status of each client
individually.  There are additional problems with this because clients
are free to 'stop reading' at any time and thus a single client
unattaching will end up forcing the hub into 'as slow as possible'
mode.  */

/* To avoid completely freezing a hub, there is still a default "go
ahead" time even when clients have not caught up.  This time is
difficult to assess literally because it is a repeated sleep loop so
the os may perform many activities during the sleep.  Extending the
length of this delay time increases the safety guarantee for lagging
clients, but also increases the potential for lag and for
molasses-like shells after remote clients disconnect.  */

/* In general for the standard use of interactive shells paranoid mode
is unnecessary and all of this can and should be ignored.  For data
critical applications aiming for high speed constant throughput,
paranoid mode can and should be used, but additional data
verification, such as a cryptographic hashing protocol, would still be
recommended.  */
