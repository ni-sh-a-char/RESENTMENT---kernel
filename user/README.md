# The RESENTMENT userspace

Everything in this directory becomes the initial ramdisk, mounted at `/boot`.

- `etc/boot.she` runs before the first prompt. It is trusted by position and
  starts with every permission; edit it to configure the machine.
- `bin/*.she` are ordinary scripts. Run one with `.run /boot/bin/hello.she`.

Scripts start with **no permissions at all**. When one tries to do something it
was not granted, the kernel refuses at the capability check and the error names
the exact flag that would allow it:

```
resentment> .run /boot/bin/agent.she
not allowed to system.
  This script was not granted permission to read the runtime graph.
  Run it with --allow-graph to permit it.

resentment> .allow graph
granted permission to read the runtime graph
```

That refusal is not the interpreter being careful. It is the same capability
check a compiled binary hits, in the same place, for the same reason.
