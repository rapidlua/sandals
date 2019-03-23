#! /bin/sh
systemd-run --user --pipe -E WD=$(pwd) sh -c \
'CGRP=/sys/fs/cgroup$$(cat /proc/self/cgroup|sed -e s/0:://);\
mkdir $CGRP/mgmt &&\
echo $$$$ > $CGRP/mgmt/cgroup.procs &&\
echo +pids > $CGRP/cgroup.subtree_control &&\
$WD/sandbozo'
