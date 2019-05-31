OBJS+=jstr/jstr.o src/cgroup.o src/fail.o src/file.o src/jshelper.o
OBJS+=src/mounts.o src/net.o src/pipes.o src/request.o src/response.o
OBJS+=src/sandals.o src/spawner.o src/stdstreams.o src/supervisor.o
OBJS+=src/usrgrp.o

CFLAGS?=-Os -DNDEBUG
CFLAGS+=-I.

build: sandals stdstreams_helper.so
test: build
	nodejs tests/run.js
	nodejs tests/socket.js

sandals: ${OBJS} kafel/libkafel.a
	${CC} ${LDFLAGS} -o sandals $^

install: sandals
	install -Ds sandals ${DESTDIR}${PREFIX}/bin/sandals

clean:
	rm -fv sandbal ${OBJS} 
	rm -fv stdstreams_helper.so ${STDSTREAMS_HELPER_OBJS} musl.flags
	make -C kafel clean

kafel/libkafel.a:
	make -C kafel

STDSTREAMS_HELPER_OBJS+=src/stdstreams_helper.o src/stdstreams.o
STDSTREAMS_HELPER_OBJS+= hook_engine/hook_engine.o hook_engine/hde/hde64.o

stdstreams_helper.so: CFLAGS+=-fpic -fvisibility=hidden
src/stdstreams.o: CFLAGS+=-fpic
src/stdstreams_helper.o: musl.flags
src/stdstreams_helper.o: CFLAGS+=-include musl.flags

stdstreams_helper.so: ${STDSTREAMS_HELPER_OBJS}
	$(CC) $^ $(CPPFLAGS) $(CFLAGS) -o $@ -shared -Wl,-init,init

install_helper: stdstreams_helper.so
	install -Ds stdstreams_helper.so ${DESTDIR}${PREFIX}/lib/sandals/stdstreams_helper.so

musl.flags:
	(echo 'int main(){void __stdio_write();__stdio_write();}' \
	| gcc -x c -o /dev/null - 2>/dev/null) && echo "#define MUSL 1" > musl.flags; \
	touch musl.flags

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

src/stdstreams_helper.o: src/stdstreams.h
