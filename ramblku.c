#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

#include <stdio.h>
#include <ctype.h>
#include <fcntl.h>
#include <zlib.h>

#define BLKSZ 4096	/* uncompressed block size */

#define SWAP_MAGIC	"SWAPSPACE2"
#define SWAP_MAGIC_LEN	10
#define SWAP_PAGE_SIZE	4096

static char Einvalid[] = "invalid control";
static char Eunknown[] = "unknown file";
static char Eperm[] = "permission denied";
static char Enomem[] = "out of memory";
static char Erange[] = "offset out of range";

typedef struct {
	uchar *cbuf;	/* compressed block, nil if all zeros */
	ulong csize;	/* valid compressed bytes */
} Blk;

typedef struct {
	Blk *blks;	/* block table */
	ulong nblks;	/* number of allocated blocks */
	ulong dsize;	/* uncompressed logical size (highwater) */
	ulong cap;	/* uncompressed capacity (bytes) */
	int swap;	/* 1 if data starts with a swap header */
	Lock lk;	/* guards all above */
} Ram;

static File *data;
static File *ctl;

static void dataread(Req *r);
static void ctlread(Req *r);
static void datawrite(Req *r);
static void ctlwrite(Req *r);

int
threadmaybackground(void)
{
	return 1;
}

/* compress src[0:srclen] into a freshly malloc'd buffer (*dstp),
 * return compressed length */
static ulong
deflatemem(uchar *src, ulong srclen, uchar **dstp)
{
	uLongf dstlen;
	int ret;

	if(srclen == 0)
	{
		*dstp = nil;
		return 0;
	}
	dstlen = compressBound(srclen);
	*dstp = malloc(dstlen);
	if(*dstp == nil)
		return 0;
	ret = compress(*dstp, &dstlen, src, srclen);
	if(ret != Z_OK)
	{
		free(*dstp);
		*dstp = nil;
		return 0;
	}
	return dstlen;
}

/* inflate src[0:srclen] into a fresh dstlen-byte buffer;
 * return dstlen, or 0 on error */
static ulong
inflatemem(uchar *src, ulong srclen, ulong dstlen, uchar **dstp)
{
	uLongf dlen;
	int ret;

	*dstp = malloc(dstlen ? dstlen : 1);
	if(*dstp == nil)
		return 0;
	if(srclen == 0)
	{
		memset(*dstp, 0, dstlen ? dstlen : 1);
		return dstlen;
	}
	dlen = dstlen ? dstlen : 1;
	ret = uncompress(*dstp, &dlen, src, srclen);
	if(ret != Z_OK)
	{
		free(*dstp);
		*dstp = nil;
		return 0;
	}
	return dlen;
}

/* grow block table so index lastidx is valid */
static int
blkgrow(Ram *ram, ulong lastidx)
{
	Blk *nb;
	ulong oldn;

	if(lastidx < ram->nblks)
		return 1;
	oldn = ram->nblks;
	nb = realloc(ram->blks, (lastidx + 1) * sizeof *nb);
	if(nb == nil)
		return 0;
	ram->blks = nb;
	while(oldn <= lastidx)
	{
		ram->blks[oldn].cbuf = nil;
		ram->blks[oldn].csize = 0;
		oldn++;
	}
	ram->nblks = lastidx + 1;
	return 1;
}

/* inflate block i into a fresh BLKSZ buffer; nil if error */
static uchar *
blkinflate(Ram *ram, ulong i)
{
	uchar *buf;

	buf = malloc(BLKSZ);
	if(buf == nil)
		return nil;
	if(ram->blks[i].cbuf == nil)
	{
		memset(buf, 0, BLKSZ);
		return buf;
	}
	if(inflatemem(ram->blks[i].cbuf, ram->blks[i].csize, BLKSZ, &buf) == 0)
	{
		free(buf);
		return nil;
	}
	return buf;
}

/* read n bytes from /dev/urandom */
static int
readurandom(uchar *buf, int n)
{
	int fd, nread;

	fd = open("/dev/urandom", OREAD);
	if(fd < 0)
		return -1;
	nread = read(fd, buf, n);
	close(fd);
	return nread == n ? 0 : -1;
}

/* write a Linux swap v1 header at page 0 */
static int
swapheader(Ram *ram)
{
	uchar header[SWAP_PAGE_SIZE];
	int pages;
	int ver;
	int lastpage;
	int firstpage;
	uchar *blk;
	uchar *oldcbuf;
	ulong oldcsize;

	if(ram->cap < SWAP_PAGE_SIZE)
		return -1;

	memset(header, 0, SWAP_PAGE_SIZE);

	/* version = 1 at offset 1024 */
	ver = 1;
	memcpy(header + 1024, &ver, 4);

	/* page counts at offsets 1028/1032 */
	pages = ram->cap / SWAP_PAGE_SIZE;
	lastpage = pages - 1;
	firstpage = 0;
	memcpy(header + 1028, &lastpage, 4);
	memcpy(header + 1032, &firstpage, 4);

	/* uuid (16 bytes) at offset 1036 */
	if(readurandom(header + 1036, 16) < 0)
		return -1;

	/* volume name (32 bytes) at offset 1052 */
	memcpy(header + 1052, "ramblk", 6);

	/* magic "SWAPSPACE2" at offset 4086 */
	memcpy(header + 4086, SWAP_MAGIC, SWAP_MAGIC_LEN);

	/* ensure block 0 exists */
	if(blkgrow(ram, 0) == 0)
		return -1;

	/* inflate block 0, overlay header, recompress */
	blk = blkinflate(ram, 0);
	if(blk == nil)
		return -1;
	memcpy(blk, header, SWAP_PAGE_SIZE);

	oldcbuf = ram->blks[0].cbuf;
	oldcsize = ram->blks[0].csize;
	ram->blks[0].cbuf = nil;
	ram->blks[0].csize = 0;
	ram->blks[0].csize = deflatemem(blk, BLKSZ, &ram->blks[0].cbuf);
	if(ram->blks[0].csize == 0 && ram->blks[0].cbuf == nil)
	{
		/* compression failed; restore old block */
		ram->blks[0].cbuf = oldcbuf;
		ram->blks[0].csize = oldcsize;
		free(blk);
		return -1;
	}
	free(oldcbuf);
	free(blk);

	if(ram->dsize < SWAP_PAGE_SIZE)
		ram->dsize = ram->cap;

	ram->swap = 1;
	return 0;
}

static void
fsread(Req *r)
{
	File *f;

	f = r->fid->file;
	if(f == data)
	{
		dataread(r);
		return;
	}
	if(f == ctl)
	{
		ctlread(r);
		return;
	}
	respond(r, Eunknown);
}

static void
ctlread(Req *r)
{
	File *f;
	Ram *ram;
	char buf[64];
	int total;
	int n;
	ulong off;

	f = r->fid->file;
	ram = f->aux;
	lock(&ram->lk);
	off = r->ifcall.offset;
	total = snprint(buf,
			sizeof buf,
			"cap %lud\n"
			"used %lud\n"
			"swap %d\n",
			ram->cap,
			ram->dsize,
			ram->swap);
	if(total < 0)
		total = 0;
	if((ulong) off >= (ulong) total)
	{
		unlock(&ram->lk);
		r->ofcall.count = 0;
		respond(r, nil);
		return;
	}
	n = total - off;
	if((ulong) n > r->ifcall.count)
		n = r->ifcall.count;
	memcpy(r->ofcall.data, buf + off, n);
	unlock(&ram->lk);
	r->ofcall.count = n;
	respond(r, nil);
}

static void
dataread(Req *r)
{
	File *f;
	Ram *ram;
	ulong off;
	ulong count;
	uchar *buf;
	ulong i;
	ulong j;
	ulong b;
	ulong bs;
	ulong be;
	uchar *blk;

	f = r->fid->file;
	ram = f->aux;
	lock(&ram->lk);
	off = r->ifcall.offset;
	count = r->ifcall.count;
	if(count == 0 || off >= ram->dsize)
	{
		unlock(&ram->lk);
		r->ofcall.count = 0;
		respond(r, nil);
		return;
	}
	if(count > ram->dsize - off)
		count = ram->dsize - off;
	buf = malloc(count);
	if(buf == nil)
	{
		unlock(&ram->lk);
		respond(r, Enomem);
		return;
	}
	memset(buf, 0, count);
	i = off / BLKSZ;
	j = (off + count - 1) / BLKSZ;
	for(b = i; b <= j; b++)
	{
		if(b >= ram->nblks)
			continue;
		blk = blkinflate(ram, b);
		if(blk == nil)
		{
			free(buf);
			unlock(&ram->lk);
			respond(r, Einvalid);
			return;
		}
		bs = b * BLKSZ;
		be = bs + BLKSZ;
		if(off > bs)
			bs = off;
		if(off + count < be)
			be = off + count;
		if(be > ram->dsize)
			be = ram->dsize;
		if(be > bs)
			memcpy(buf + (bs - off),
			       blk + (bs - b * BLKSZ),
			       be - bs);
		free(blk);
	}
	memcpy(r->ofcall.data, buf, count);
	free(buf);
	unlock(&ram->lk);
	r->ofcall.count = count;
	respond(r, nil);
}

static void
fswrite(Req *r)
{
	File *f;

	f = r->fid->file;
	if(f == data)
	{
		datawrite(r);
		return;
	}
	if(f == ctl)
	{
		ctlwrite(r);
		return;
	}
	respond(r, Eunknown);
}

static void
ctlwrite(Req *r)
{
	File *f;
	Ram *ram;
	char *ptr;
	ulong mb;
	ulong newcap;
	ulong count;
	ulong b;

	f = r->fid->file;
	count = r->ifcall.count;
	ram = f->aux;
	lock(&ram->lk);
	if(r->ifcall.count == 0 || r->ifcall.count >= 16)
	{
		unlock(&ram->lk);
		respond(r, Einvalid);
		return;
	}
	ptr = malloc(r->ifcall.count + 1);
	if(ptr == nil)
	{
		unlock(&ram->lk);
		respond(r, Enomem);
		return;
	}
	memcpy(ptr, r->ifcall.data, r->ifcall.count);
	ptr[r->ifcall.count] = '\0';
	if(strchr(ptr, '\n') != nil)
		ptr[r->ifcall.count - 1] = '\0';

	/* "swap" formats page 0 as a Linux swap v1 header */
	if(strcmp(ptr, "swap") == 0)
	{
		free(ptr);
		if(ram->cap < SWAP_PAGE_SIZE)
		{
			unlock(&ram->lk);
			respond(r, Einvalid);
			return;
		}
		if(swapheader(ram) < 0)
		{
			unlock(&ram->lk);
			respond(r, Enomem);
			return;
		}
		unlock(&ram->lk);
		r->ofcall.count = count;
		respond(r, nil);
		return;
	}

	/* "noswap" clears the swap flag */
	if(strcmp(ptr, "noswap") == 0)
	{
		free(ptr);
		ram->swap = 0;
		unlock(&ram->lk);
		r->ofcall.count = count;
		respond(r, nil);
		return;
	}

	if(sscanf(ptr, "%lu", &mb) <= 0 || mb == 0)
	{
		free(ptr);
		unlock(&ram->lk);
		respond(r, Einvalid);
		return;
	}
	free(ptr);
	if(mb > (~0UL / (1024 * 1024)))
	{
		unlock(&ram->lk);
		respond(r, Einvalid);
		return;
	}
	newcap = mb * 1024 * 1024;
	for(b = 0; b < ram->nblks; b++)
		free(ram->blks[b].cbuf);
	free(ram->blks);
	ram->blks = nil;
	ram->nblks = 0;
	ram->dsize = 0;
	ram->cap = newcap;
	data->dir.length = ram->cap;
	unlock(&ram->lk);
	r->ofcall.count = count;
	respond(r, nil);
}

static void
datawrite(Req *r)
{
	File *f;
	Ram *ram;
	ulong off;
	ulong count;
	ulong newdsize;
	ulong i;
	ulong j;
	ulong b;
	ulong nneed;
	ulong bs;
	ulong be;
	uchar *blk;
	uchar *oldcbuf;
	ulong oldcsize;

	f = r->fid->file;
	ram = f->aux;
	lock(&ram->lk);
	off = r->ifcall.offset;
	count = r->ifcall.count;
	if(ram->swap)
	{
		unlock(&ram->lk);
		respond(r, Einvalid);
		return;
	}
	if(count == 0 || off >= ram->cap)
	{
		unlock(&ram->lk);
		respond(r, Erange);
		return;
	}
	if(count > ram->cap - off)
	{
		unlock(&ram->lk);
		respond(r, Erange);
		return;
	}
	newdsize = ram->dsize;
	if(off + count > newdsize)
		newdsize = off + count;
	nneed = (newdsize + BLKSZ - 1) / BLKSZ;
	if(nneed > ram->nblks && !blkgrow(ram, nneed - 1))
	{
		unlock(&ram->lk);
		respond(r, Enomem);
		return;
	}
	i = off / BLKSZ;
	j = (off + count - 1) / BLKSZ;
	for(b = i; b <= j; b++)
	{
		blk = blkinflate(ram, b);
		if(blk == nil)
		{
			unlock(&ram->lk);
			respond(r, Enomem);
			return;
		}
		bs = b * BLKSZ;
		be = bs + BLKSZ;
		if(off > bs)
			bs = off;
		if(off + count < be)
			be = off + count;
		memcpy(blk + (bs - b * BLKSZ),
		       r->ifcall.data + (bs - off),
		       be - bs);
		oldcbuf = ram->blks[b].cbuf;
		oldcsize = ram->blks[b].csize;
		ram->blks[b].cbuf = nil;
		ram->blks[b].csize = 0;
		ram->blks[b].csize = deflatemem(blk,
						BLKSZ,
						&ram->blks[b].cbuf);
		if(ram->blks[b].csize == 0 && ram->blks[b].cbuf == nil)
		{
			/* compression failed; restore old block */
			ram->blks[b].cbuf = oldcbuf;
			ram->blks[b].csize = oldcsize;
			free(blk);
			unlock(&ram->lk);
			respond(r, Enomem);
			return;
		}
		free(oldcbuf);
		free(blk);
	}
	ram->dsize = newdsize;
	data->dir.length = ram->cap;
	unlock(&ram->lk);
	r->ofcall.count = count;
	respond(r, nil);
}

static void
fscreate(Req *r)
{
	respond(r, Eperm);
}

static void
fsremove(Req *r)
{
	respond(r, Eperm);
}

static void
fswstat(Req *r)
{
	respond(r, nil);
}

static Srv fs = {
	.read=		fsread,
	.write=		fswrite,
	.create=	fscreate,
	.remove=	fsremove,
	.wstat=		fswstat
};

void
threadmain(int argc, char *argv[])
{
	char *srvname;
	Ram *ram;

	ram = malloc(sizeof *ram);
	if(ram == nil)
		threadexits("malloc");
	memset(ram, 0, sizeof *ram);
	ram->cap = 16 * 1024 * 1024;	/* 16 MB default capacity */
	ram->swap = 0;
	fs.tree = alloctree(nil, nil, 0777 | DMDIR, nil);
	data = createfile(fs.tree->root, "data", nil, 0664, ram);
	data->dir.length = ram->cap;
	ctl = createfile(fs.tree->root, "ctl", nil, 0660, ram);
	srvname = "ramblk";
	threadpostmountsrv(&fs, srvname, nil, MBEFORE);
	threadexits(nil);
}
