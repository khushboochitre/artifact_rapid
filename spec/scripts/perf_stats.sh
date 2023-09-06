#!/bin/bash
MIMALLCOC="$HOME/artifact_rapid/mimalloc_region"
MIMALLCOC_PROF="$HOME/artifact_rapid/mimalloc_prof"
export LD_LIBRARY_PATH=$MIMALLCOC:$MIMALLCOC_PROF:$LD_LIBRARY_PATH
SPECDIR=$HOME/artifact_rapid/cpuspec

source shrc
SIZE="ref"
ITER=1
BENCHMARK="544.nab_r 538.imagick_r 525.x264_r 500.perlbench_r 502.gcc_r 520.omnetpp_r 523.xalancbmk_r 531.deepsjeng_r 508.namd_r 510.parest_r 511.povray_r 526.blender_r"

echo "Perf Statistics"
for val in $BENCHMARK
do
	export FILENAME=$val
	runcpu --config=native_perf --noreportable --tune=base --copies=1 --iteration=1 --action=clobber --size=$SIZE $val >> output.txt
	runcpu --config=native_perf --noreportable --tune=base --copies=1 --iteration=$ITER --size=$SIZE $val >> output.txt
	
	runcpu --config=allocator_perf --noreportable --tune=base --copies=1 --iteration=1 --action=clobber --size=$SIZE $val >> output.txt
	runcpu --config=allocator_perf --noreportable --tune=base --copies=1 --iteration=$ITER --size=$SIZE $val >> output.txt

	runcpu --config=rapid_perf --noreportable --tune=base --copies=1 --iteration=1 --action=clobber --size=$SIZE $val >> output.txt
	runcpu --config=rapid_perf --noreportable --tune=base --copies=1 --iteration=$ITER --size=$SIZE $val >> output.txt
done
