# CPU scheduling on the A55/A76 clusters

SC7180 has six Cortex-A55 cores (cluster 0, CPU0-5) and two Cortex-A76 cores (cluster 1, CPU6-7). Until now every core sat in
pset 0 under the single-cluster Clutch scheduler, because a second pset without `__AMP__` panicked in `thread_select`
(`sched_clutch_steal_thread` returns with the pset lock held); `coachz-prelink-smp.patch` only made idle-core selection prefer
the A76s. The scheduler could not tell the clusters apart and nothing moved a running thread to a big core.

## Edge scheduler (`coachz-prelink-amp.patch`)

On hardware: `COACHZ sched: Edge, 2 clusters, FIXPRI/FG/IN prefer P cluster 1, DF/UT/BG cluster 0`, and all seven secondary
cores come up.

- `__ARM_AMP__` is defined for this board in `board_config.h`, so `osfmk/kern/sched.h` builds Clutch with the Edge policy
  (`CONFIG_SCHED_EDGE`), `processor.c` sets up `pset_node0` for the boot (E) cluster and `pset_node1` for the other, and
  `ml_processor_register` finds or creates the pset of each core's cluster again (`pset_find(cluster_id)`). The topology already
  has two clusters: without `HAS_CLUSTER` XNU numbers them by `cluster-type` from the ADT (`E` for CPU0-5, `P` for CPU6-7).
- Edge reads its cluster edges in `sched_edge_cpu_init_completed`, which XNU reaches through `ml_cpu_init_completed()`; Apple's
  `AppleARMSMP` calls it once all CPUs are started. ArmCpu (`patches/drivers/ArmCpu.patch`, shared with coachz-screen) now
  counts secondaries that return from `processor_start` and calls it when all of the topology's secondaries are in the scheduler.
  On the non-AMP route the scheduler has no such callback and the call does nothing.
- Apple's default edges allow P to E migration (weight 64) and forbid E to P; CLPC then recommends a cluster per thread group
  and QoS. Nothing here runs CLPC, and every new thread group starts on pset0 (the A55s), so without a recommendation no thread
  would ever reach an A76. Once the clusters are up, the fixed-priority, foreground and user-initiated buckets prefer the P
  cluster and default, utility and background the E cluster, for existing thread groups (`thread_group_coachz_foreach` with
  `sched_edge_tg_preferred_cluster_change`) and new ones (`sched_clutch_bucket_group_init`). Before that every bucket stays on
  the boot cluster, the only pset sure to exist. The E to P edge allows stealing (still no migration), so an idle A76 takes
  runnable work from the A55 queues; P to E spill keeps Apple's weight.
- The smp patch's preferred-bitmask hints become no-ops: each pset is all-P or all-E.
- Log: `COACHZ SMP: all 7 secondary cores in the scheduler, ml_cpu_init_completed` and `COACHZ sched: Edge, 2 clusters, ...`.

Host checks: the full series applies with `git apply` from the pinned XNU; every file with `__AMP__` code (scheduler, recount,
processor, thread, memorystatus, sysctl, vm_pageout, platform tests) passes a syntax check with its own compile record and
`__ARM_AMP__`/`CONFIG_SCHED_EDGE` in effect; ArmCpu.cpp passes with its component command.

To check on hardware: both log lines; K36/K38 per-core load with the A76s carrying the foreground (render server, SpringBoard)
and the A55s the background; no `thread_select` or pset lock panic.

## Switches for an early stall

`tools/xnu_screen.py --cpu-smp-skip BITS` (/chosen `armcpu-smp-skip`) leaves parts of ArmCpu's all-cores-up step out without
a rebuild: bit 0 `ml_cpu_init_completed` (the Edge policy's cluster setup), bit 1 the PSCI queries. The line
`COACHZ SMP: all 7 secondary cores in the scheduler, ml_cpu_init_completed[ skipped]` shows which ran. The CPU5 offline test
already has its own switch (`--cpu-offline-test`). k224's first boot stopped before the disk log opened, with
"rootvnode is not set" as the last screen line, which in a normal boot sits between the secondaries' CPU_ON lines; that this
step is the cause is a suspicion, not a result.
