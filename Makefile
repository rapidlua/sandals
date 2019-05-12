OBJS+=jstr/jstr.o src/cgroup.o src/fail.o src/file.o src/jshelper.o
OBJS+=src/mounts.o src/net.o src/pipes.o src/request.o src/response.o
OBJS+=src/sandals.o src/spawner.o src/stdstreams.o src/supervisor.o
OBJS+=src/usrgrp.o

CFLAGS?=
CFLAGS+=-I.

sandals: ${OBJS} kafel/libkafel.a
	${CC} ${LDFLAGS} -o sandals $^

install: sandals
	install -Ds sandals ${DESTDIR}${PREFIX}/bin/sandals

clean:
	rm -fv sandbal ${OBJS}
	make -C kafel clean

run: sandals
	./run.sh

kafel/libkafel.a:
	make -C kafel

jstr/jstr.o: jstr/jstr.h
src/cgroup.o: jstr/jstr.h src/sandals.h src/jshelper.h
src/fail.o: jstr/jstr.h src/sandals.h
src/file.o: jstr/jstr.h src/sandals.h src/jshelper.h
src/jshelper.o: jstr/jstr.h src/sandals.h src/jshelper.h
src/mounts.o: jstr/jstr.h src/sandals.h src/jshelper.h
src/net.o: jstr/jstr.h src/sandals.h
src/pipes.o: jstr/jstr.h src/sandals.h src/jshelper.h
src/request.o: jstr/jstr.h src/sandals.h src/jshelper.h
src/response.o: jstr/jstr.h src/sandals.h
src/sandals.o: jstr/jstr.h src/sandals.h
src/spawner.o: jstr/jstr.h src/sandals.h src/stdstreams.h
src/stdstreams.o: jstr/jstr.h src/sandals.h src/stdstreams.h
src/supervisor.o: jstr/jstr.h src/sandals.h src/stdstreams.h
src/usrgrp.o: jstr/jstr.h src/sandals.o
