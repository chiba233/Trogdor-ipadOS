# Writable external Data

The 64 MiB volatile Data seed is too small for the original services now running. Use a dedicated
8 GiB, role-less APFS container as the fourth GPT partition after kernel, OS and SystemOS. Preserve
the original seed skeleton and ownership while resizing an ordinary image on macOS. A full image
is written locally on macOS and read back; later kernel prefixes must match that exact GPT.

The existing USB block device remains the only device publishing the containers. An explicit
main-OS boot argument selects persistent Data and its APFS volume UUID. Validate both GPT copies,
all partition bounds and CRCs, the fourth partition's type and exact size before admitting writes.
Every write must fall wholly inside that partition. All other sectors reject writes, including OS,
SystemOS, boot partition and GPT. Use the existing single device I/O lock for reads, writes and a
real SCSI SYNCHRONIZE CACHE command. Report failures rather than claiming durable completion.

Mount only the selected UUID under that partition read-write at /private/var before launchd. A
missing or invalid selected Data device is fatal; do not silently fall back to the tiny memory
seed. Keep the earlier memory Data and restore routes available when persistent mode is absent.
The kernel prepares empty keybag directories and the existing narrowly scoped watchdog policy on
whichever Data route was selected. No changes to Apple user binaries or signature policy.

Host checks cover the actual partition validator, protected-sector writes, bounds/overflow,
corrupt GPTs and truncated I/O, plus unchanged baseline payload routes. Inspect the generated GPT,
boot arguments and embedded kernel before writing. Hardware acceptance requires the 8 GiB Data
mount and original service progress; building and writing are prerequisites, not acceptance.
The hardware run has since reached a later FrontBoard display assertion: mandatory pre-launchd
union and selected external Data gates passed. Debian confirms the 8 GiB partition. File-content
persistence across reset remains a separate unverified property; see docs/hardware-tests.md.
