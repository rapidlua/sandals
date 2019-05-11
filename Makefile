OBJS+=jstr/jstr.o src/cgroup.o src/fail.o src/file.o src/jshelper.o
OBJS+=src/mounts.o src/net.o src/pipes.o src/request.o src/response.o
OBJS+=src/sandals.o src/spawner.o src/stdstreams.o src/supervisor.o
OBJS+=src/usrgrp.o

CFLAGS?=
CFLAGS+=-I.

sandals: ${OBJS} kafel/libkafel.a
	${CC} ${LDFLAGS} -o sandals $^

clean:
	rm -fv sandbal ${OBJS}
	make -C kafel clean

run: sandals
	./run.sh

kafel/libkafel.a:
	make -C kafel
