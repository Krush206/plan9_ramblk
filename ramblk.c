#include <u.h>
#include <libc.h>
#include <auth.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include <pool.h>
#include <stdio.h>
#include <ctype.h>

typedef struct {
	uchar *buf;
	ulong size;
} Ram;

static char Einvctl[] = "invalid control";
static char Eunkfile[] = "unknown file";
static char Eperm[] = "permission denied";

static File *data;
static File *ctl;

static void
fsread(Req *r)
{
	File *f;

	f = r->fid->file;
	if(f == data)
	{
		long offset;
		long count;
		Ram *ram;

		offset = r->ifcall.offset;
		count = r->ifcall.count;
		ram = r->fid->file->aux;
		if(offset + count > ram->size)
		{
			r->ofcall.count = 0;
			respond(r, nil);
			return;
		}

		memcpy(r->ofcall.data, ram->buf + offset, count);
		r->ofcall.count = count;
		respond(r, nil);
		return;
	}
	if(f == ctl)
	{
		r->ofcall.count = 0;
		respond(r, nil);
		return;
	}
	respond(r, Eunkfile);
}

static void
fswrite(Req *r)
{
	File *f;

	f = r->fid->file;
	if(f == data)
	{
		long offset;
		long count;
		Ram *ram;

		offset = r->ifcall.offset;
		count = r->ifcall.count;
		ram = f->aux;
		if(offset + count > ram->size)
		{
			r->ofcall.count = 0;
			respond(r, nil);
			return;
		}

		memcpy(ram->buf + offset, r->ifcall.data, count);
		r->ofcall.count = count;
		respond(r, nil);
		return;
	}
	if(f == ctl)
	{
		char count[8];
		char value[8];
		Ram *ram;

		if(r->ifcall.count >= sizeof count || r->ifcall.count == 0)
			goto Invctl;
		ram = f->aux;
		sprintf(count, "%%.%lds", r->ifcall.count);
		sprintf(value, count, r->ifcall.data);
		if(sscanf(value, "%lu", &ram->size) <= 0)
			goto Invctl;
		free(ram->buf);
		ram->size = 1024 * 1024 * ram->size;
		ram->buf = calloc(1, ram->size);
		data->length = ram->size;
		r->ofcall.count = 0;
		respond(r, nil);
		return;
	}
	respond(r, Eunkfile);
	return;
Invctl:
	respond(r, Einvctl);
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
usage(void)
{
	fprint(2, "usage: %s size\n", argv0);
	exits("usage");
}

void
main(int argc, char *argv[])
{
	char *srvname;
	char *ptr;
	Ram *ram;
	File *root;

	argv0 = argv[0];
	if(argc != 2)
		usage();
	ptr = argv[1];
	while(*ptr != '\0')
		if(!isdigit(*ptr++))
			usage();
	ptr = argv[1];
	ram = malloc(sizeof *ram);
	sscanf(ptr, "%lu", &ram->size);
	ram->size = 1024 * 1024 * ram->size;
	ram->buf = calloc(1, ram->size);
	fs.tree = alloctree(nil, nil, 0777 | DMDIR, nil);
	root = fs.tree->root;
	data = createfile(root, "data", nil, 0664, ram);
	data->length = ram->size;
	ctl = createfile(root, "ctl", nil, 0660, ram);
	srvname = "ramblk";
	postmountsrv(&fs, srvname, nil, MBEFORE);
	exits(0);
}
