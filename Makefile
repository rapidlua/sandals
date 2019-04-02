OBJS += sandbozo.o request.o response.o fail.o file.o net.o mounts.o
OBJS += cgroup.o pipes.o usrgrp.o supervisor.o spawner.o jstr/jstr.o

sandbozo: ${OBJS}

clean:
	rm -fv sandbozo ${OBJS}

run: sandbozo
	./run.sh
