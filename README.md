# sandals
A lightweight process isolation tool for Linux.
It is built using Linux namespaces, cgroups v2, and seccomp-bpf syscall filters.

```
$ echo '{"cmd":["ps","-A"],
         "pipes":[{"dest":"/dev/stdout","stdout":true}],
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
 to filesystem quota in effect; e.g. a subprocess terminated due to exceeded memory limit.
 A task integrity is compromised; it is unlikely to recover and it might produce bogus
 results if not prepared to handle an unusual failure. The compromised task is terminated
 and detailed status tells the reason and gives a hint if certain limits should be increased.
 1. **rootless** — No privileges required. No suid binaries involved.
 1. **as fast as possible** — Wrapping a task in `docker run`
 [reportedly](https://medium.com/travis-on-docker/the-overhead-of-docker-run-f2f06d47c9f3)
adds 300ms overhead. Sandals reduces that to a mere 5ms, a boon for short-lived tasks!
 1. **exposes Linux kernel features instead of inventing higher-level abstractions** —
 Primarily a side effect of being fast and lean. We aren't Docker,
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

## Installation

```
$ make && make install
```

## User guide

(This guide is not meant to be exhaustive, check [Reference](#Reference) for further details.)

Sandals runs untrusted code in a private set of *Linux namespaces*.
Namespaces partition system objects. E.g. **IPC namespaces** partition IPC objects.
Sandals creates the full set of private namespaces by default. Sandboxed
code is unable to interact with host processes, either directly via signals or indirectly
via IPC or sockets. It has no access to the network either.

### Restricting access to the filesystem

*TODO*

### Limiting resource usage with cgroups

*TODO*

## Reference

### Response

JSON object with `status` key. Depending on the status aditional keys might be present:

 * **exited**: exited normally
   * **code**: process exit code
 * **killed**: killed by signal
   * **signal**: killing signal (string, e.g. `SIGTERM`)
 * **memoryLimit**: memory limit as set by cgroup's `memory.max` exceeded
 * **pidsLimit**: pids limit as set by cgroup's `pids.max` exceeded
 * **timeLimit**: run time limit exceeded, see `timeLimit` in [Request](#Request) section
 * **fileLimit**: task output is written to a file; file size limit exceeded, see `pipes` in [Request](#Request) section
 * **requestInvalid**: request JSON invalid
    * **description**: error description
 * **internalError**: internal error
    * **description**: error description

### Request

JSON object with the mandatory `cmd` key.
Ex: `{"cmd":["uname","-a"]}`.

Optional keys:

 * **hostName**: string
   
   Host name as observed inside sandbox. Default: `sandals`.
 
 * **domainName**: string
 
   Domain name as observed inside sandbox. Default: `sandals`.

 * **uid**: number
 
   User id as observed inside sandbox. Default: `0`.
 
 * **gid**: number

   Group id as observed inside sandbox. Default: `0`.

 * **chroot**: string
 
   A path to use as filesystem root inside sandbox. Default: `/`.
 
 * **mounts**: object []
 
   A list of mounts to apply to filesystem view inside sandbox. Default: `[]`.
   
   Every mount has the mandatory `type` key. Type is either `bind`
   for a bind mount or a value recognized by `mount` system call,
   like `tmpfs` or `proc`.
   
   The mandatory `dest` key tells the destination of the mount.
   Chroot prefix (if any) is automatically prepended to the given path.
  
   Bind mounts must specify the source path with `src` key.
   
   Optional `options` string (empty by default) is passed to `mount` syscall verbatim.
   
   Optional `ro` boolean key (default: `false`) turns on read-only mode for the mount.
  
 * **cgroupConfig**: object
 
   Enable resource limiting with cgroups. Default: cgroups not used. 
 
   Ex: `{"memory.max": "1000000", "memory.swap.max": "1000000"}`.
   For each key/value pair, the `value` is written to the file named by `key` in the cgroup directory.
   
 * **cgroup**, **cgroupRoot**: string
 
   If `cgroupConfig` is present, sandboxed task is put into a separate cgroup.
   This is either an existing cgroup as specified by `cgroup` key or
   a new one if the later key is absent.
   
   A new cgroup is created under `cgroupRoot` if present, otherwize a new
   cgroup is spawned as a sibling of the current cgroup.
   
   If a new cgroup was created it is removed when sandals exits.
 
 * **seccompPolicy**: string
 
   A syscall filtering policy in Kafel syntax. Filtering disabled by default.
 
 * **vaRandomize**: boolean
  
   ASLR, enabled by default.
 
 * **env**: string []
 
   Task's environment as a list of `KEY=VALUE` strings. Empty by default.
 
 * **workDir**: string
 
   Task's working directory. Default: `/`.
   
   Chroot prefix (if any) is automatically prepended to the given path.
 
 * **timeLimit**: number
 
   A time limit in seconds. No limit by default.
 
 * **pipes**: object []
 
   A list of unidirectional channels for streaming data out of the sandbox.
   
   Mandatory `dest` key  names the destination file to write the data.
   
   At least one of `stdout`, `stderr` or `src` keys must be present.
   If `stdout` key is set to `true` the pipe is attached as a task's standard output.
   Set `stderr` to `true` to attach the standard error stream.
   The last option is to expose the pipe ingress as a fifo in the filesystem.
   Chroot prefix (if any) is automatically prepended to the path in `src` key.
   
   Optional `limit` numeric key caps the maximum amount of collected data (no limit by default).
   If the limit is exeeded, the task terminates with `status:fileLimit`.

 * **copyFiles**: object []

   A list of files to copy out of the sandbox once it terminates.

   Subkeys are the same as in pipe object, see `pipes`.

 * **stdStreams**: object
 
   Subkeys (same meaning as in pipe object, see `pipes`):
   
   * **dest**
   
   * **limit**
 
   Capture both stdout and stderr simultaneously.
   Use case: present task's output *exactly* as it would appear if invoked in a terminal.
   Style stderr content differently (eg: red color).
   
   Every chunk of data is prefixed with 32-bit integer in the network byte order.
   Bits 0..30 encode the chunk length. Bit 31 tells whether the chunk origin is stdout (0) or stderr (1).
      
   **Note:** if size limit is exceeded the last packet (header + chunk) might be cut short.

   **Note:** *capturing task's output __exactly__ as it would appear if invoked in a terminal*, while
   distinguishing stdout/stderr content, is tricky.
   In most programming languages, if a stream is writing to a terminal, a different buffering strategy is used.
   If the same pipe is attached to stdout and stderr, it won't be possible to tell which stream the particular
   octet originated from. If two pipes are used, preserving relative ordering of chunks is challenging.
   
   The current implementation utilizes UNIX datagram sockets, which solve the ordering problem nicely.
   The caveat is that any write to stdout/sdterr exceeding the maximum datagram size (~200KiB) will fail.
   Also streams buffering doesn't match.
   
   If the perfect solution is desired, we suggest installing a custom Linux kernel module
   ([available separately](https://github.com/mejedi/proxyfd)).
   Sandals will use the kernel module if present.
