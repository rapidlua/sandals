OBJS += sandals.o request.o response.o fail.o file.o net.o mounts.o
OBJS += cgroup.o pipes.o usrgrp.o supervisor.o spawner.o jstr/jstr.o

sandals: ${OBJS}

clean:
	rm -fv sandbal ${OBJS}

run: sandals
	./run.sh
