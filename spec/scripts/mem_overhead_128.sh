#!/bin/bash
MIMALLCOC="$HOME/artifact_rapid/mimalloc_region"
MIMALLCOC_PROF="$HOME/artifact_rapid/mimalloc_prof"
export LD_LIBRARY_PATH=$MIMALLCOC:$MIMALLCOC_PROF:$LD_LIBRARY_PATH
SPECDIR=$HOME/artifact_rapid/cpuspec

source shrc
SIZE="ref"
ITER=1
THRESH=128
BENCHMARK="523.xalancbmk_r 510.parest_r 526.blender_r"

echo "Memory overhead"
cd $SPECDIR/result && rm -r *
for val in $BENCHMARK
do
	export FILENAME=$val
  export THRESHOLD=$THRESH
	rm -r $HOME/stats/"$FILENAME"_native_mem_overhead_"$THRESHOLD".txt
	runcpu --config=native_mem_overhead_threshold --noreportable --tune=base --copies=1 --iteration=1 --action=clobber --size=$SIZE $val >> output.txt
	runcpu --config=native_mem_overhead_threshold --noreportable --tune=base --copies=1 --iteration=$ITER --size=$SIZE $val >> output.txt
	
	rm -r $HOME/stats/"$FILENAME"_rapid_mem_overhead_"$THRESHOLD".txt
	runcpu --config=rapid_mem_overhead_threshold --noreportable --tune=base --copies=1 --iteration=1 --action=clobber --size=$SIZE $val >> output.txt
	runcpu --config=rapid_mem_overhead_threshold --noreportable --tune=base --copies=1 --iteration=$ITER --size=$SIZE $val >> output.txt

	file1="$HOME"/stats/"$FILENAME"_native_mem_overhead_"$THRESHOLD".txt
	file2="$HOME"/stats/"$FILENAME"_rapid_mem_overhead_"$THRESHOLD".txt
	mem1=0	
	for input in $file1
	do
		while IFS= read -r line;
		do
			read -a arr <<< $line
			if [ "${arr[0]}" -gt $mem1 ];
			then
				mem1=${arr[0]}
			fi
		done < "$input"
	done

	mem2=0	
	for input in $file2
	do
		while IFS= read -r line;
		do
			read -a arr <<< $line
			if [ "${arr[0]}" -gt $mem2 ];
			then
				mem2=${arr[0]}
			fi
		done < "$input"
	done

	overhead=`bc <<<"scale=2; ((($mem2-$mem1)*100) / $mem1)"`
    echo $val $overhead
done
