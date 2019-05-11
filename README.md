# sandals
A lightweight process isolation tool for Linux.
It is built using Linux namespaces, cgroups v2, and seccomp-bpf syscall filters.

```
$ echo '{"cmd":["ps","-A"],
         "pipes":[{"file":"/dev/stdout","stdout":true}],
         "mounts":[{"type":"proc","dest":"/proc"}]
        }' | sandals
  PID TTY          TIME CMD
    1 ?        00:00:00 sandals
    2 ?        00:00:00 ps
{"status":"exited","code":0}
```

## Highlights

 1. **speaks JSON** — A departure from long-standing UNIX tradition, `sandals` reads JSON request
 from `STDIN` and writes JSON response to `STDOUT`, instead of relying on program arguments and
 exit code.
 1. **detailed status** — Response tells whether the task exited normally or was killed
 or terminated due to compromised integrity (see below).
 1. **task integrity** — Sandbox introduces new modes of failure, e.g. disk full error due to
 to filesystem quota in effect; a subprocess terminated due to exceeded memory limit.
 A task integrity is compromised; it is unlikely to recover and it might produce bogus
 results if not prepared to handle an unusual failure. The compromised task is terminated
 and detailed status tells the reason and gives a hint if certain limits should be increased.
 1. **rootless** — No privileges required. No suid binaries involved.
 1. **as fast as possible** — Wrapping a task in `docker run`
 [reportedly](https://medium.com/travis-on-docker/the-overhead-of-docker-run-f2f06d47c9f3)
adds 300ms overhead. Sandals reduces that to a mere 5ms, a boon for short-lived tasks!
 1. **exposes Linux kernel features instead of inventing new abstractions** — We aren't Docker,
 the user should be sufficiently versed in namespaces, cgroups, seccomp and mounts.
 
Other mature lightweight sandboxes are readily available:
[isolate](https://github.com/ioi/isolate) (est. 2013),
[firejail](https://firejail.wordpress.com) (est. 2014),
[nsjail](https://github.com/google/nsjail) (est. 2015)
to name a few. They are similarly low-level and comparably fast (though we managed to 
innovatively squeeze almost 20ms by doing namespaced process shutdown asynchronously).
Our other features are unparalleled though.

Most notably, existing solutions require elevated privileges
in order to set up a constrained sandbox. This is an inherent limitation
of cgroups v1. Sandals, on the other hand, takes advantage of cgroups v2.

## Isolation

Employed Linux kernel isolation features are listed below.
A feature is enabled unconditionally, unless the converse is explicitly stated. 

 * *namespaces*
   * *user namespace*
   * *pid namespace* — task executes in a brand-new pid namespace with `sandals` as PID 1;
   * *net namespace* — no network, only a private loopback interface is accessible;
   * *mount namespace*
   * *ipc namespace*
   * *uts namespace*
   * *cgroup namespace* — iff `cgroupConfig` is supplied;
 * *cgroups* — iff `cgroupConfig` is supplied;
 * *seccomp* — iff `seccompPolicy` is supplied.
