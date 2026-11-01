# Coachz WCN3990: pending platform inventory and WLFW wire code

This directory is not consumed by the kernel build. Nothing here is in `series`, `board.toml`, `kernel_source.py` or the active loader. The owner will wire in and compile the candidate. No kernel or XNU object was compiled for this work, and there is no hardware result.

## Implemented

| File | Purpose |
|---|---|
| `coachz_wifi.c` | Optional `coachz-wifi=1` sampler hook. Reads the SMEM partition table, APPS/MODEM and global partition metadata, the incoming MPSS SMP2P item and existing GLINK descriptor. Does not write shared memory or acknowledge/consume a queue. |
| `coachz_wifi_smem.h` | Portable SMEM private-partition/item decoder, including cached items and padding; incoming SMP2P decoding. Other host pairs are skipped before their partition headers are read. |
| `coachz_wifi_proto.h` | Little-endian QMI/TLV parsing and builders for WLFW indication registration, capability and target MSA-info requests; response/result validation, capability decoding and QRTR v1 WLFW service announcements. No transport. |
| `adt.py` | An explicit helper for the existing loader's `Node` API. It is not imported by the active loader. |
| `coachz-prelink-wifi.patch` | Self-contained candidate patch: the three C/header files, XNU file registration and one sampler call. Not applied and not in `series`. |

The direct source files are canonical. The candidate patch contains byte-identical copies, checked by the host test. Recreate that patch when these files change; do not edit the embedded copies independently.

## What stage 1 observes

SC7180 SMEM is `0x80900000 + 0x200000`, unlike the Pixel reference. The probe requires both an explicit ADT node and this exact range. It refuses a range overlapping the current `boot_args` physical-memory window. This is an additional check, not a substitute for confirming the original FDT's reserved-memory data.

The public `$TOC` partition-table format (version 1, used by SMEM v12) is supported. There is no legacy global-heap fallback. A different format produces `CW_UNSUPPORTED` instead of speculative reads. Only the global and APPS/MODEM partitions are inspected; protected ADSP/SLPI partitions are never dereferenced.

The incoming SMP2P item is 435, from MODEM host 1 to APPS host 0 (smp2p.c reads `qcom,smem = <435>, <428>` as inbound, outbound; 428 is the AP's). `slave-kernel` is reported as raw state, with version/features/flags. These can be left over from firmware: the snapshot does not establish a live remote processor or ready WLFW endpoint. The existing GLINK items are 478 (descriptor), 479 (APPS TX) and 480 (APPS RX). Missing allocations are normal on a cold boot before MPSS starts.

No hardware lock, SMEM allocation, GLINK negotiation, FIFO pointer update, mailbox write, SCM call, reset, power vote, radio MMIO or DMA is performed. The mapped SMEM range stays mapped for the kernel lifetime. The inventory runs once on the existing sampler thread.

## Optional wiring, performed by the owner

1. Confirm the original Coachz FDT/SKU and SMEM range. Keep all remote-processor regions out of XNU allocations. The present inventory only reads SMEM; later boot/DMA stages must separately reserve MPSS, MBA, MSA, metadata and any host DMA pool.
2. Copy `coachz-prelink-wifi.patch` to the XNU patch directory under the same name, then append that name to `series`. Its contexts are the accepted EC file registration and sampler's local `ticks` declaration, so it can be placed after the accepted battery/input patches. It does not depend on the optional GPU patch.
3. In a private loader candidate, call `add_wifi_inventory(root, smem_base, smem_size)` from `adt.py` after `prelink_tree()` has parsed `root`, before `root.write()` determines the final serialized size. Pass the checked FDT values; do not insert the node into an already packed image. The normal prelinked path in `pack()` is the intended caller.
4. Add `coachz-wifi=1` only to that candidate's kernel command line. Without it, the sampler hook returns before looking up the ADT or mapping memory.
5. Build/package through the normal project route and perform hardware acceptance separately. This work did not run those steps.

An illustrative loader call after explicitly importing the helper is:

```python
add_wifi_inventory(root, checked_smem_base, checked_smem_size)
```

The helper does not reserve RAM or validate a live FDT. The owner must connect it at the correct pre-serialization point; there is deliberately no automatic loader patch or default boot argument here.

## Expected diagnostics after integration

All lines start with `COACHZ wifi:` and use the existing kernel log route. Look for:

- `SMEM v12 inventory status 0`: the table and selected partition headers were parsed.
- `global ...` and `apps-modem ...`, or a clear `partition absent` line.
- Incoming SMP2P item status, or its `slave`/raw `state` snapshot.
- Each GLINK item status/offset/size; if a descriptor exists, TX/RX indices and whether both FIFO bounds are known.
- `inventory complete; MPSS/WLFW liveness and radio not tested`.

Status codes are `0 OK`, `1 absent`, `2 format`, `3 bounds`, `4 read callback failed`, `5 QMI remote failure`, `6 unsupported layout`. A successful inventory is not a scan, link or Wi-Fi-driver milestone. Do not expect an interface or a Wi-Fi menu change from this candidate.

## Next implementation boundary

The next substantial step is the SC7180 MBA/PIL/MPSS startup and memory-permission path, then SMEM allocation under the hardware mutex, SMP2P/GLINK negotiation, QRTR and an actual WLFW response. The wire helpers are ready to be used by that transport; this candidate does not send the requests. In particular, constructing an MSA request does not establish ownership of its memory.

After the control plane works: install the right firmware/BDF, establish firmware and CE DMA SMMU domains, then adapt the same-chip Flame CE/HTC/WMI/HTT data plane. Scanning precedes WPA2 and the Darwin 23 Apple80211/ifnet bridge. No placeholder MAC, copied Pixel calibration, SSID, credentials, firmware or Apple binary is included.

## Host verification

```sh
python3 -B -m unittest discover -s tests -p test_wifi.py -v
```

This compiles only the portable fixture program with the host C compiler and uses temporary files for the patch test. It does not compile the XNU-facing `coachz_wifi.c`, invoke a kernel build, assemble a boot image or touch a device. The tests exercise wire bytes, partition selection, item decoding, the real loader's ADT serialization, candidate-patch consistency and the disabled integration boundary.

See [SOURCES.md](SOURCES.md) for pinned references and [the plan](../../../../docs/plans/wifi.md) for the Chinese handoff.
