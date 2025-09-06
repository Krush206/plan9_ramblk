#include <u.h>
#include <libc.h>
#include <auth.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include <pool.h>

void
fsread(Req *r)
{
	respond(r, nil);
}

void
fswrite(Req *r)
{
	respond(r, nil);
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
fsopen(Req *r)
{
	respond(r, nil);
}

void
fsattach(Req *r)
{
	print("OK\n");
	respond(r, nil);
}

void
fsstart(Srv *s)
{
	File *root;

	root = s->tree;
	root = alloctree(nil, nil, 0777 | DMDIR, nil);
	createfile(root, "data", getuser(), 0664, nil);
	createfile(root, "ctl", getuser(), 0660, nil);
}

Srv fs = {
	.open=		fsopen,
	.read=		fsread,
	.write=		fswrite,
	.create=	fscreate,
	.remove=	fsremove,
	.attach=	fsattach,
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
