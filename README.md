# sandals
A lightweight process isolation tool for Linux.
It is built using Linux namespaces, cgroups v2, and seccomp-bpf syscall filters. 

`docker run`
[reportedly](https://medium.com/travis-on-docker/the-overhead-of-docker-run-f2f06d47c9f3)
incurs approximately 300ms of the overhead.
This is prohibitively high for a short lived task. Sandals reduces that to 5ms.

Other mature lightweight sandboxes are readily available:
[isolate](https://github.com/ioi/isolate) (est. 2013),
[firejail](https://firejail.wordpress.com) (est. 2014),
[nsjail](https://github.com/google/nsjail) (est. 2015)
to name a few. Every single one of them require elevated privileges
in order to set up a constrained sandbox. This is an inherent limitation
of cgroups v1.

Sandals, on the other hand, is totally usable by an unpriviliged user thanks to cgroups v2.
