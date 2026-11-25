# Repository Agent Instructions

## Kernel code review

When reviewing, modifying, or reasoning about kernel code under `src/`, read and
follow the kernel safety skill at:

- `.codex/skills/kernel-code-check/SKILL.md`

Pay particular attention to user-memory copies, blocking calls, and ring-buffer
`u=1` operations inside spinlock/irqsave regions.

Before committing changes under `src/`, run:

```sh
python3 .codex/skills/kernel-code-check/scripts/check_kernel_locks.py src
make kernel
```

Fix scanner findings and build failures before committing.

## Commits

Never skip git hooks when committing. Do not use `--no-verify`, `--no-gpg-sign`,
or any other option that bypasses `pre-commit` or `commit-msg` hooks. If a hook
fails, fix the reported issue and commit again without disabling the hook.
