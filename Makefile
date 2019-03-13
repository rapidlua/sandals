sandbozo: sandbozo.o helpers.o jstr/jstr.o

run: sandbozo
	systemd-run --user --tty -E WD=$$(pwd) sh -c \
	'CGRP=/sys/fs/cgroup$$(cat /proc/self/cgroup|sed -e s/0:://);\
	mkdir $$CGRP/mgmt &&\
	echo $$$$$$$$ > $$CGRP/mgmt/cgroup.procs &&\
	echo +pids > $$CGRP/cgroup.subtree_control &&\
	$$WD/sandbozo'
