# hook-engine
Minimalistic hook engine for x86_64/Linux

## Example:

Imagine we'd like to hook `read` function from libc:

```c
ssize_t read(int fd, void *buf, size_t count);
```

Whenever `read` is called, we want to transfer control to
`__wrap__read` (our custom function), as if `read` was 
defined as follows:

```c
ssize_t read(int fd, void *buf, size_t count) {
  return __wrap__read(fd, buf, count);
}
```

`Hook-engine` literally patches `read`
code in memory. Since `read` was altered, we need
another function that performs identically to unmodified
`read`. This is called *a trampoline*, and `hook-engine`
can create one.

Let's start with a trampoline definition:

```c
ssize_t __real__read(int fd, void *buf, size_t count);
HOOK_DEFINE_TRAMPOLINE(__real__read);
```

`HOOK_DEFINE_TRAMPOLINE` reserves space in the
program's `code` segment. `Hook-engine` renders tramoline's code there at runtime.

Now we can implement fake descriptor `#42`, yielding
a quote from Douglas Adams:

```c
ssize_t __wrap__read(int fd, void *buf, size_t count) {
  if (fd == 42) {
    static const char msg[] = "answer to life the universe and everything";
    if (count >= sizeof msg) count = (sizeof msg) - 1;
    memcpy(buf, msg, count);
    return count;
  }
  return __real__read(fd, buf, count);
}
```

Finally install the hook and render the trampoline:
```c
if (hook_install(read, __wrap__read, __real__read) != 0) {
  fprintf(stderr, "%s\n", hook_last_error());
  exit(EXIT_FAILURE);
}
```
