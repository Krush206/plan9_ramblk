#include <u.h>
#include <libc.h>
#include <auth.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include <pool.h>

typedef struct {
	uchar *buf;
	ulong size;
} Ram;

static File *data;
static File *ctl;

void
fsread(Req *r)
{
	if(r->fid->file == data)
	{
		long off;
		long cnt;
		Ram *ram;

		off = r->ifcall.offset;
		cnt = r->ifcall.count;
		ram = r->fid->file->aux;
		if(off >= ram->size)
			cnt = 0;
		else if(off + cnt > ram->size)
			cnt = ram->size - off;

		memmove(r->ofcall.data, ram->buf + off, cnt);
		r->ofcall.count = cnt;
		respond(r, nil);
		return;
	}
	respond(r, "unknown file");
}

void
fswrite(Req *r)
{
	if(r->fid->file == data)
	{
		long off;
		long cnt;
		Ram *ram;

		off = r->ifcall.offset;
		cnt = r->ifcall.count;
		ram = r->fid->file->aux;
		if(off >= ram->size)
			cnt = 0;
		else if(off + cnt > ram->size)
			cnt = ram->size - off;

		memmove(ram->buf + off, r->ifcall.data, cnt);
		r->ofcall.count = cnt;
		respond(r, nil);
		return;
	}
	respond(r, "unknown file");
}

void
fscreate(Req *r)
{
	respond(r, "permission denied");
}

void
fsremove(Req *r)
{
	respond(r, "permission denied");
}

void
fsstart(Srv *s)
{
	File *root;
	Ram *dataRam;

	s->tree = alloctree(nil, nil, 0777 | DMDIR, nil);
	root = s->tree->root;
	dataRam = malloc(sizeof *dataRam);
	dataRam->size = 1024 * 1024 * 128;
	dataRam->buf = malloc(dataRam->size);
	data = createfile(root, "data", nil, 0664, dataRam);
	ctl = createfile(root, "ctl", nil, 0660, nil);
}

Srv fs = {
	.read=		fsread,
	.write=		fswrite,
	.create=	fscreate,
	.remove=	fsremove,
	.start=		fsstart
};

void
main(int argc, char **argv)
{
	char *srvname;

	srvname = "ramblk";
	postmountsrv(&fs, srvname, nil, MBEFORE);
	exits(0);
}
