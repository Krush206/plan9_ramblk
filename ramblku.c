#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

#include <stdio.h>
#include <ctype.h>
#include <fcntl.h>
#include <zlib.h>

#define SWAP_MAGIC		"SWAPSPACE2"
#define SWAP_MAGIC_LEN	10
#define SWAP_PAGE_SIZE	4096

#define CPAGESIZE		4096
#define CPAGEMAX		8192	/* safely above compressBound(4096) */

typedef struct {
	ulong csize;	/* compressed size, 0 if page unwritten (all zeros) */
	ulong off;	/* offset in cbuf */
	int uncomp;	/* 1 if stored uncompressed (raw page) */
} CPage;

typedef struct {
	uchar *data;	/* full uncompressed data (compress off) */
	uchar *scratch;	/* one-page scratch (compress on) */
	uchar *cbuf;	/* compressed storage (compress on) */
	CPage *cpages;	/* page table (compress on) */
	ulong npages;	/* logical page count */
	ulong size;	/* logical/uncompressed size */
	int swap;
	int compress;
	Lock lk;
} Ram;

static char Einvalid[] = "invalid control";
static char Eunknown[] = "unknown file";
static char Eperm[] = "permission denied";
static char Enomem[] = "out of memory";

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

/* decompress logical page p into scratch (one full page) */
static int
decompresspage(Ram *ram, ulong p, uchar *scratch)
{
	CPage *cp;
	ulong len;

	cp = &ram->cpages[p];
	if(cp->csize == 0)
	{
		memset(scratch, 0, CPAGESIZE);
		return 0;
	}
	if(cp->uncomp)
	{
		memcpy(scratch, ram->cbuf + cp->off, CPAGESIZE);
		return 0;
	}
	len = CPAGESIZE;
	if(uncompress(scratch, &len, ram->cbuf + cp->off, cp->csize) != Z_OK)
		return -1;
	return 0;
}

/* compress scratch (one full page) into logical page p */
static int
compresspage(Ram *ram, ulong p, uchar *scratch)
{
	CPage *cp;
	ulong len;

	cp = &ram->cpages[p];
	len = CPAGEMAX;
	if(compress2(ram->cbuf + cp->off,
		     &len,
		     scratch,
		     CPAGESIZE,
		     Z_BEST_SPEED) != Z_OK)
		return -1;
	cp->csize = len;
	cp->uncomp = 0;
	return 0;
}

/* store src uncompressed into logical page p (used for swap header page) */
static void
storepageuncomp(Ram *ram, ulong p, uchar *src)
{
	CPage *cp;

	cp = &ram->cpages[p];
	memcpy(ram->cbuf + cp->off, src, CPAGESIZE);
	cp->csize = CPAGESIZE;
	cp->uncomp = 1;
}

/* write a Linux swap v1 header at page 0 */
static int
writeswapheader(Ram *ram)
{
	uchar header[SWAP_PAGE_SIZE];
	int pages;
	int ver;
	int lastpage;
	int firstpage;

	if(ram->size < SWAP_PAGE_SIZE)
		return -1;

	memset(header, 0, SWAP_PAGE_SIZE);

	/* version = 1 at offset 1024 */
	ver = 1;
	memcpy(header + 1024, &ver, 4);

	/* page counts at offsets 1028/1032 */
	pages = ram->size / SWAP_PAGE_SIZE;
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

	if(ram->compress)
		storepageuncomp(ram, 0, header);
	else
		memcpy(ram->data, header, SWAP_PAGE_SIZE);

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
dataread(Req *r)
{
	File *f;
	Ram *ram;
	vlong offset;
	ulong count;
	uchar *dst;
	vlong cur;
	ulong remaining;
	ulong p;
	ulong pgoff;
	ulong n;

	f = r->fid->file;
	ram = f->aux;
	lock(&ram->lk);
	offset = r->ifcall.offset;
	count = r->ifcall.count;
	if(count == 0 ||
	   offset > ram->size ||
	   ram->size - offset < count)
	{
		unlock(&ram->lk);
		r->ofcall.count = 0;
		respond(r, nil);
		return;
	}

	dst = (uchar *) r->ofcall.data;
	cur = offset;
	remaining = count;
	while(remaining > 0)
	{
		p = cur / CPAGESIZE;
		pgoff = cur % CPAGESIZE;
		n = remaining;
		if(n > CPAGESIZE - pgoff)
			n = CPAGESIZE - pgoff;
		if(ram->compress)
		{
			if(decompresspage(ram, p, ram->scratch) < 0)
			{
				unlock(&ram->lk);
				respond(r, Einvalid);
				return;
			}
			memcpy(dst, ram->scratch + pgoff, n);
		}
		else
			memcpy(dst, ram->data + cur, n);
		dst += n;
		cur += n;
		remaining -= n;
	}
	r->ofcall.count = count;
	unlock(&ram->lk);
	respond(r, nil);
}

static void
ctlread(Req *r)
{
	File *f;
	Ram *ram;
	char buf[64];
	int n;
	vlong avail;

	f = r->fid->file;
	ram = f->aux;
	lock(&ram->lk);
	n = snprint(buf,
		    sizeof buf,
		    "cap %lud\n"
		    "swap %d\n"
		    "compress %d\n",
		    ram->size,
		    ram->swap,
		    ram->compress);
	if(n < 0)
		n = 0;
	avail = n - r->ifcall.offset;
	if(avail < 0)
		avail = 0;
	if((vlong) r->ifcall.count > avail)
		r->ofcall.count = avail;
	else
		r->ofcall.count = r->ifcall.count;
	if(r->ofcall.count > 0)
		memcpy(r->ofcall.data,
		       buf + r->ifcall.offset,
		       r->ofcall.count);
	unlock(&ram->lk);
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
datawrite(Req *r)
{
	File *f;
	Ram *ram;
	vlong offset;
	ulong count;
	uchar *src;
	vlong cur;
	ulong remaining;
	ulong p;
	ulong pgoff;
	ulong n;

	f = r->fid->file;
	ram = f->aux;
	lock(&ram->lk);
	offset = r->ifcall.offset;
	count = r->ifcall.count;
	if(count == 0 ||
	   offset > ram->size ||
	   ram->size - offset < count)
	{
		unlock(&ram->lk);
		r->ofcall.count = 0;
		respond(r, nil);
		return;
	}

	src = (uchar *) r->ifcall.data;
	cur = offset;
	remaining = count;
	while(remaining > 0)
	{
		p = cur / CPAGESIZE;
		pgoff = cur % CPAGESIZE;
		n = remaining;
		if(n > CPAGESIZE - pgoff)
			n = CPAGESIZE - pgoff;
		if(ram->compress)
		{
			if(decompresspage(ram, p, ram->scratch) < 0)
			{
				unlock(&ram->lk);
				respond(r, Einvalid);
				return;
			}
			memcpy(ram->scratch + pgoff, src, n);
			if(compresspage(ram, p, ram->scratch) < 0)
			{
				unlock(&ram->lk);
				respond(r, Enomem);
				return;
			}
		}
		else
			memcpy(ram->data + cur, src, n);
		src += n;
		cur += n;
		remaining -= n;
	}
	r->ofcall.count = count;
	unlock(&ram->lk);
	respond(r, nil);
	return;
}

static void
ctlwrite(Req *r)
{
	File *f;
	Ram *ram;
	char *ptr;
	ulong mb;
	ulong nsize;
	ulong npages;
	ulong i;

	f = r->fid->file;
	if(r->ifcall.count == 0 || r->ifcall.count >= 64)
	{
		respond(r, Einvalid);
		return;
	}
	ptr = malloc(r->ifcall.count + 1);
	if(ptr == nil)
	{
		respond(r, Enomem);
		return;
	}
	memcpy(ptr, r->ifcall.data, r->ifcall.count);
	ptr[r->ifcall.count] = '\0';

	/* "swap" formats the beginning of data as swap */
	if(strcmp(ptr, "swap") == 0)
	{
		free(ptr);
		ram = f->aux;
		lock(&ram->lk);
		if(ram->size < SWAP_PAGE_SIZE)
		{
			unlock(&ram->lk);
			respond(r, Einvalid);
			return;
		}
		if(writeswapheader(ram) < 0)
		{
			unlock(&ram->lk);
			respond(r, Enomem);
			return;
		}
		ram->swap = 1;
		unlock(&ram->lk);
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;
	}

	/* "noswap" clears the swap flag */
	if(strcmp(ptr, "noswap") == 0)
	{
		free(ptr);
		ram = f->aux;
		lock(&ram->lk);
		ram->swap = 0;
		unlock(&ram->lk);
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;
	}

	/* "compress" enables zlib page compression */
	if(strcmp(ptr, "compress") == 0)
	{
		free(ptr);
		ram = f->aux;
		lock(&ram->lk);
		if(ram->compress)
		{
			unlock(&ram->lk);
			r->ofcall.count = r->ifcall.count;
			respond(r, nil);
			return;
		}
		ram->npages = (ram->size + CPAGESIZE - 1) / CPAGESIZE;
		ram->scratch = malloc(CPAGESIZE);
		ram->cbuf = nil;
		ram->cpages = nil;
		if(ram->npages > 0)
		{
			ram->cbuf = malloc(ram->npages * CPAGEMAX);
			ram->cpages = malloc(ram->npages * sizeof(CPage));
		}
		if(ram->scratch == nil ||
		   (ram->npages > 0 &&
		    (ram->cbuf == nil ||
		     ram->cpages == nil)))
		{
			free(ram->scratch);
			free(ram->cbuf);
			free(ram->cpages);
			ram->scratch = nil;
			ram->cbuf = nil;
			ram->cpages = nil;
			ram->npages = 0;
			unlock(&ram->lk);
			respond(r, Enomem);
			return;
		}
		memset(ram->cpages, 0, ram->npages * sizeof(CPage));
		for(i = 0; i < ram->npages; i++)
			ram->cpages[i].off = i * CPAGEMAX;
		for(i = 0; i < ram->npages; i++)
		{
			uchar *pg;
			ulong pgsize;
			pg = ram->data + i * CPAGESIZE;
			pgsize = CPAGESIZE;
			if(i * CPAGESIZE + CPAGESIZE > ram->size)
				pgsize = ram->size - i * CPAGESIZE;
			memset(ram->scratch, 0, CPAGESIZE);
			memcpy(ram->scratch, pg, pgsize);
			compresspage(ram, i, ram->scratch);
		}
		free(ram->data);
		ram->data = nil;
		ram->compress = 1;
		unlock(&ram->lk);
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;
	}

	/* "nocompress" disables zlib page compression */
	if(strcmp(ptr, "nocompress") == 0)
	{
		free(ptr);
		ram = f->aux;
		lock(&ram->lk);
		if(!ram->compress)
		{
			unlock(&ram->lk);
			r->ofcall.count = r->ifcall.count;
			respond(r, nil);
			return;
		}
		ram->data = malloc(ram->size);
		if(ram->data == nil)
		{
			unlock(&ram->lk);
			respond(r, Enomem);
			return;
		}
		for(i = 0; i < ram->npages; i++)
		{
			uchar *pg;
			ulong pgsize;
			pg = ram->data + i * CPAGESIZE;
			pgsize = CPAGESIZE;
			if(i * CPAGESIZE + CPAGESIZE > ram->size)
				pgsize = ram->size - i * CPAGESIZE;
			memset(ram->scratch, 0, CPAGESIZE);
			if(decompresspage(ram, i, ram->scratch) < 0)
			{
				free(ram->data);
				ram->data = nil;
				unlock(&ram->lk);
				respond(r, Einvalid);
				return;
			}
			memcpy(pg, ram->scratch, pgsize);
		}
		free(ram->scratch);
		free(ram->cbuf);
		free(ram->cpages);
		ram->scratch = nil;
		ram->cbuf = nil;
		ram->cpages = nil;
		ram->npages = 0;
		ram->compress = 0;
		unlock(&ram->lk);
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;
	}

	free(ptr);

	/* otherwise it must be a size in MB */
	if(sscanf(ptr, "%lu", &mb) <= 0)
	{
		respond(r, Einvalid);
		return;
	}

	if(mb > (~0UL / (1024UL * 1024UL)))
	{
		respond(r, Einvalid);
		return;
	}

	ram = f->aux;
	lock(&ram->lk);
	nsize = mb * 1024 * 1024;
	npages = (nsize + CPAGESIZE - 1) / CPAGESIZE;

	if(ram->compress)
	{
		uchar *ncbuf;
		CPage *ncpages;

		ncbuf = nil;
		ncpages = nil;
		if(npages > 0)
		{
			ncbuf = realloc(ram->cbuf, npages * CPAGEMAX);
			ncpages = realloc(ram->cpages,
					  npages * sizeof(CPage));
		}
		if(npages > 0 && (ncbuf == nil || ncpages == nil))
		{
			unlock(&ram->lk);
			respond(r, Enomem);
			return;
		}
		if(npages > ram->npages)
			for(i = ram->npages; i < npages; i++)
			{
				ncpages[i].csize = 0;
				ncpages[i].off = i * CPAGEMAX;
				ncpages[i].uncomp = 0;
			}
		ram->cbuf = ncbuf;
		ram->cpages = ncpages;
		ram->npages = npages;
	}
	else
	{
		uchar *ndata;

		ndata = nil;
		if(nsize > 0)
			ndata = realloc(ram->data, nsize);
		if(nsize > 0 && ndata == nil)
		{
			unlock(&ram->lk);
			respond(r, Enomem);
			return;
		}
		if(nsize > ram->size)
			memset(ndata + ram->size,
			       0,
			       nsize - ram->size);
		ram->data = ndata;
	}
	ram->size = nsize;
	data->dir.length = ram->size;
	ram->swap = 0;
	unlock(&ram->lk);
	r->ofcall.count = r->ifcall.count;
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

static void
usage(void)
{
	fprint(2, "usage: %s size\n", argv0);
	threadexits("usage");
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
	char *ptr;
	Ram *ram;
	File *root;
	ulong mb;

	argv0 = argv[0];
	if(argc != 2)
		usage();
	ptr = argv[1];
	while(*ptr != '\0')
		if(!isdigit(*ptr++))
			usage();
	ptr = argv[1];
	if(sscanf(ptr, "%lu", &mb) <= 0 || mb == 0)
		usage();
	if(mb > (~0UL / (1024UL * 1024UL)))
		usage();

	ram = malloc(sizeof *ram);
	if(ram == nil)
		threadexits("malloc");
	memset(ram, 0, sizeof *ram);
	ram->size = mb * 1024 * 1024;
	ram->data = calloc(1, ram->size);
	if(ram->data == nil)
	{
		free(ram);
		threadexits("malloc");
	}
	ram->compress = 0;
	ram->swap = 0;
	ram->scratch = nil;
	ram->cbuf = nil;
	ram->cpages = nil;
	ram->npages = 0;

	fs.tree = alloctree(nil, nil, 0777 | DMDIR, nil);
	root = fs.tree->root;
	data = createfile(root, "data", nil, 0664, ram);
	data->dir.length = ram->size;
	ctl = createfile(root, "ctl", nil, 0660, ram);
	srvname = "ramblk";
	threadpostmountsrv(&fs, srvname, nil, MBEFORE);
	threadexits(nil);
}
