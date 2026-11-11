# Files and Safari download diagnosis

Files operation and Safari download completion remain unaccepted. A WebKit download-finished message alone does not establish that
the downloaded file can be listed, opened or retained through a restart.

## Current hardware evidence

The k212 log ring contains eighteen samples of the original `fseventsd` initialization worker at the same wait path, across two
daemon instances. Against the pinned OS executable's instructions, the worker has completed its disk-initialization call, found a
null global logger and entered a loop calling `sleep(86400)`. Main creates this worker after its Mach check-in and connection setup;
the samples therefore do not support the earlier hypothesis that the daemon never checked in. Clients in this boot report invalid
destination ports when registering their FSEvent streams.

The persistent Data's `.fseventsd` directory contains its UUID file but no event journals in the collected view;
`/private/var/db/fseventsd` is absent. The read-only source OS and SystemOS roots have no `.fseventsd` directory. These directory
observations do not identify the vnode opened by the native daemon or its returned filesystem metadata.

The original daemon's historical `<1>` failure covers several outcomes: directory `fstatfs` failure, a mismatch between the opened
directory's fsid and the enumerated volume, or a root-volume entry inconsistent with the device information. The current union root
has separate underlying OS, SystemOS and Data mounts. A mount listing alone cannot distinguish these branches; transferring ROOTFS
flags or fabricating a volume UUID would be an unsupported fix.

Separately, k212 contains 159 PlugInKit containment rejections. The pinned PlugInKitDaemon rejection branch takes a missing containing
application URL from LaunchServices, then applies its `/System/` fallback check. This identifies missing parent metadata; it does not
prove that every rejected extension caused the local-storage provider failure. The previous database-rebuild marker did not resolve
the rejections. A further marker bump is not a demonstrated repair.

In the same boot, PlugInKit successfully creates `com.apple.FileProvider.LocalStorage` at its canonical `/System` path;
`fileproviderd` starts its domains, and file coordination adds that provider. The generic containment rejection is therefore not
evidence that LocalStorage itself was rejected.

Earlier download attempts log `unable to retrieve UUID for device`, followed by `can't get volume UUID` and notifier creation failure.
The original LocalStorage executable resolves what that message means: its device helper calls `FSEventsCopyUUIDForDevice` and emits
the first error when the call returns null. This is an FSEvents UUID query, not direct evidence of a missing APFS volume UUID.
The daemon's initialization and RPC availability are thus a concrete dependency to investigate before changing volume identity.
The instruction path and historical errors do not establish that it is the only Files/download failure.

## Pending boundary diagnostics

`coachz-prelink-file-services.patch` adds bounded kernel observations for `fseventsd` only:

- Opens of the root, Data and fallback event-log directories report requested/final flags, errors and successful vnode filesystem
  metadata. Paths are represented by fixed labels; filenames and UUID contents are not logged.
- Native statfs, statfs64, fstatfs64 and getfsstat64 results report the selected filesystem, actual/returned flags, logical-root
  filesystem and process mount-view policy. Failures and exhausted diagnostic budgets are explicit.
- Directory creation, unlink/rmdir and F_GETPATH record outcomes, selected filesystem metadata and fixed path labels. Both path
  resolution and copying the F_GETPATH result to the caller are observed; vnode references keep their original lifetime.
- Getattrlist records common/volume masks, extended common masks and options, plus the filesystem and a fixed label for the native
  no-firmlink path. The original daemon's path helper requests `ATTR_CMNEXT_NOFIRMLINKPATH` with `FSOPT_ATTR_CMN_EXTENDED`; that
  returned path must be distinguished from its enumerated mount name. This is not an APFS volume-UUID request.

The patch changes no syscall results, mount flags, files, UUIDs or registration policy. Path copying stays in a non-inlined leaf
helper, so its buffer does not extend the APFS call stack. The four patched C files pass source-only syntax checks. The patch ran in k213. This is diagnostic coverage, not a Files fix or an additional functional batch point.

## Acceptance boundary

Use the native open/stat results to identify the failed logger boundary before changing filesystem behavior. Check application and
extension registration through the original LaunchServices parent relationship, without suppressing PlugInKit's containment check.
Then validate Safari download progress and finish, the resulting file's presence and content through Files, open/share behavior, and
persistence after a later authorized restart. FSEvents startup, provider registration and a working download are separate results.

## k213 native boundary result

The bounded probes now resolve the root-directory failure: the native mount enumeration reports one root-name entry with device
0x100000f, while opening `/.fseventsd` succeeds on filesystem 0x100000d, the logical root reported by the same process. The native daemon
then logs its `<1>` failure for the root-name entry with device 0x100000f. This rules out a failed open for this attempt and identifies
a mount-view/selected-vnode mismatch. It does not authorize assigning another volume's UUID or transferring ROOTFS flags.

Data's log directory opens successfully on filesystem 0x1000011. The no-firmlink path queries for both root and Data also succeed;
the remaining task is to reconcile the duplicate root mount entries with the native daemon's device matching and startup selection.
Files and Safari download completion remain unaccepted.

The opened root-directory fd's native fstatfs also succeeds with filesystem 0x100000d. The original daemon's instructions at
0x10000b3cc–0x10000b3d8 compare that returned fsid with the enumerated device before taking the observed `<1>` error branch.
The subsequent sampled worker retains both previously symbolized return sites of the null-global-logger `sleep(86400)` loop.
Thus the daemon's startup remains blocked after the mismatch. Correcting the composed root's semantics still requires checking
both the logical root view and the original boot-filesystem identity; no flags or UUIDs have been rewritten in this investigation.

## Root view

The first correction followed the observed selection branch by projecting flags: statfs, getfsstat and `ATTR_VOL_MOUNTFLAGS` marked the
logical union root as ROOTFS and the covered boot filesystem as non-root. Booted together with a getattrlist device change for
extensions, it left userspace with contradictory volume identities, and the interface did not come up (see the next section).

`coachz-prelink-root-view.patch` now reports the union as the root volume it covers, as UniXNU's `macos-union-one-volume.patch` does:
getfsstat leaves the union out, statfs and fstatfs describe the root volume, and `stat` and getattrlist give the root volume's device
and fsid. fseventsd then enumerates one root entry, and the directory it opens reports the same filesystem. Mounts, flags, UUIDs and
the kernel's own view are unchanged.

On hardware fseventsd no longer logs the `<1>` failure and the Files app browses. Safari downloads into Downloads were not tested.

## Extensions rejected by PlugInKit: cause and fix

The 141 rejections cover every extension of an app on the boot filesystem's `/Applications` (Safari, Batteries, Photos, Messages,
Sidecar, ...); the 101 extensions of the deletable system apps shown through the bindfs union are accepted. PlugInKitDaemon's
`-[PKDPlugIn diagnose]` rejects an extension with a data container, no `containingUrl` and a path outside `/System/`; `containingUrl`
is `LSPlugInKitProxy.containingBundle.bundleURL`, read from the extension record's parent unit (plugin data +0xe0, accepted only for
a parent of bundle class 2). A LaunchServices database pulled from the device, opened with the host's CoreServicesStore, shows parent 0
for all 487 extensions in container 4 and a valid parent for all 105 in container 8. Container 4 is the device of `/` (0x100000d, the
SystemOS union root); container 8 is the bindfs mount.

LaunchServices assigns a node's container from its volume URL: CoreServicesInternal takes the device from `getattrlist` and the
volume from `statfs` by that device, and the boot filesystem's mount point is `/`, which is now the logical root. `_LSBundleFindWithNode`
then accepts a bundle only if the node's own volume identifier equals its container's. Files of the boot filesystem report their real
device (0x100000f), so no extension found its app and its parent stayed 0. On a device `/` and `/Applications` are one volume.

A first fix moved userspace onto the union instead: statfs gave the union `MNT_ROOTFS`, and getattrlist reported the boot
filesystem's files on the union's device while `stat` kept the real one. On hardware that boot never reached the interface: launchd
read the `CoreAudioServices/audiomxd` feature flag both ways (mediaserverd and audiomxd both claimed `com.apple.audio.AudioQueueServer`,
and mediaserverd aborted each time), and backboardd stopped after "start window server..." without opening the display or the GPU,
while SpringBoard waited for it in `BKSDisplayGetMainScreenInfo`. Every interface has to give the same answer.

`coachz-prelink-root-view.patch` now reports the union as the root volume it covers, the approach of UniXNU's
`macos-union-one-volume.patch`: getfsstat leaves the union out; statfs, fstatfs and their 64-bit forms describe the root volume;
`stat`'s `st_dev` and getattrlist's `ATTR_CMN_DEVID` and `ATTR_CMN_FSID` give the root volume's values, also for entries that
getattrlistbulk packs without a vnode. The union is recognised by `MNT_UNION` over the `VROOT` of an `MNT_ROOTFS` mount; the mounts,
`MNT_ROOTFS` and the kernel's own view are unchanged, and `FSOPT_RETURN_REALDEV` still returns the real device. `/` and the boot
filesystem's `/Applications` are then one volume, as on a device. The extensions register only when the database is built, so the
rebuild marker moves to `.coachz-ls-built-3`, which costs one rebuild of about 70 s behind the Apple logo.

On hardware the interface comes up, mediaserverd stays up, pkd logs no "plug-ins outside containing apps" rejection (141 before,
0 now) and the extensions of these apps appear again. Individual widgets and the Photos picker were not checked one by one.
