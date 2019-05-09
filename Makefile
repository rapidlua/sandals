OBJS += sandals.o request.o response.o fail.o file.o net.o mounts.o
OBJS += cgroup.o pipes.o usrgrp.o supervisor.o spawner.o jstr/jstr.o
OBJS += stdstreams.o

sandals: ${OBJS} kafel/libkafel.a

clean:
	rm -fv sandbal ${OBJS}
	make -C kafel clean

run: sandals
	./run.sh

kafel/libkafel.a:
	make -C kafel
