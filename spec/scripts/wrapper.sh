#!/bin/bash
source shrc
SIZE="ref"
BENCHMARK="544.nab_r 538.imagick_r 525.x264_r 500.perlbench_r 502.gcc_r 520.omnetpp_r 523.xalancbmk_r 531.deepsjeng_r 508.namd_r 510.parest_r 511.povray_r 526.blender_r"

for arg
do
        if [ $arg == "identify_wrappers" ];
        then
                runcpu --config=wrapper --noreportable --tune=base --copies=1 --iteration=1 --action=clobber --size=$SIZE $BENCHMARK
                runcpu --config=wrapper --noreportable --tune=base --copies=1 --iteration=1 --action=build --size=$SIZE $BENCHMARK
        else
                echo "Please specify atleast one argument.";
        fi
done

