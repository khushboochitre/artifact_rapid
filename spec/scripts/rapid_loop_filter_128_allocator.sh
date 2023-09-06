#!/bin/bash
MIMALLCOC="$HOME/artifact_rapid/mimalloc_region"
MIMALLCOC_PROF="$HOME/artifact_rapid/mimalloc_prof"
export LD_LIBRARY_PATH=$MIMALLCOC:$MIMALLCOC_PROF:$LD_LIBRARY_PATH
SPECDIR=$HOME/artifact_rapid/cpuspec

source shrc
SIZE="ref"
ITER=5
THRESH=128
ITERVAL=64
BENCHMARK="523.xalancbmk_r 510.parest_r 526.blender_r"

if [ $SIZE == "ref" ];
then
	EXT="refrate"
elif [ $SIZE == "test" ];
then
	EXT="test"
fi

function compute_improvement() {
	file1=$1 
	file2=$2
	COUNT=$3

	declare -A SUM1
	declare -A AVG1
	read_line=0
	while IFS= read -r line;
	do
		if [[ "$line" == *"--------------- -------  ---------  ---------    -------  ---------  ---------"* ]];
		then
			read_line=1
	    elif [[ "$line" == *"================================================================================="* ]]; 
	    then
	        read_line=0
	    elif [[ $read_line == 1 ]]; 
	    then
	        words=`echo $line | sed 's/^ *//g'`
	        word_count=0
	        benchname=""
	        timeval=0
	        for word in $words
	        do
	            if [ $word_count == 0 ];
	            then
	                benchname=$word
	                if [ ${SUM1[$benchname]} ];
	               	then
	                    SUM1[$benchname]=${SUM1[$benchname]}
	                else
	                    SUM1[$benchname]=0
	                fi
	                elif [ $word_count == 2 ];
	                then
	                    timeval=$word
	                if [ ${SUM1[$benchname]} ];
	                then
	                    val=`echo "${SUM1[$benchname]}+$timeval" | bc`
	                    SUM1[$benchname]=$val
	                fi
	            fi
	            ((word_count=word_count+1))
	        done
	    fi
	done < $file1

	#echo "Avg1"
	for x in "${!SUM1[@]}"; do
		res=`echo ${SUM1[$x]} '>' 0 | bc -l`
	    if [ $res == 1 ]; then
	        val=`bc <<<"scale=2; ${SUM1[$x]}/$COUNT"`
	        #printf "%s %s \n" "$x" "$val" ;
	        AVG1[$x]=$val
	    fi
	done

	declare -A SUM2
	declare -A AVG2
	read_line=0
	while IFS= read -r line;
	do
		if [[ "$line" == *"--------------- -------  ---------  ---------    -------  ---------  ---------"* ]];
		then
			read_line=1
	    elif [[ "$line" == *"================================================================================="* ]]; 
	    then
	        read_line=0
	    elif [[ $read_line == 1 ]]; 
	    then
	        words=`echo $line | sed 's/^ *//g'`
	        word_count=0
	        benchname=""
	        timeval=0
	        for word in $words
	        do
	            if [ $word_count == 0 ];
	            then
	                benchname=$word
	                if [ ${SUM2[$benchname]} ];
	               	then
	                    SUM2[$benchname]=${SUM2[$benchname]}
	                else
	                    SUM2[$benchname]=0
	                fi
	                elif [ $word_count == 2 ];
	                then
	                    timeval=$word
	                if [ ${SUM2[$benchname]} ];
	                then
	                    val=`echo "${SUM2[$benchname]}+$timeval" | bc`
	                    SUM2[$benchname]=$val
	                fi
	            fi
	            ((word_count=word_count+1))
	        done
	    fi
	done < $file2

	#echo "Avg2"
	for x in "${!SUM2[@]}"; do
	    res=`echo ${SUM2[$x]} '>' 0 | bc -l`
	    if [ $res == 1 ]; then
	        val=`bc <<<"scale=2; ${SUM2[$x]}/$COUNT"`
	        #printf "%s %s \n" "$x" "$val" ;
	        AVG2[$x]=$val
	        improvement=`bc <<<"scale=2; (((${AVG1[$x]}-${AVG2[$x]})*100) / ${AVG1[$x]})"`
        	printf "%s %s \n" "$x" "$improvement" ;
	    fi
	done
}

echo "Improvement"
for val in $BENCHMARK
do
	cd $SPECDIR/result && rm -r *
	export FILENAME=$val
	export THRESHOLD=$THRESH
  export LOOPITER=$ITERVAL
  runcpu --config=rapid_cpu_overhead_threshold --noreportable --tune=base --copies=1 --iteration=1 --action=clobber --size=$SIZE $val >> output.txt
	runcpu --config=rapid_cpu_overhead_threshold --noreportable --tune=base --copies=1 --iteration=$ITER --size=$SIZE $val >> output.txt
	
	runcpu --config=rapid_loop_filer_threshold --noreportable --tune=base --copies=1 --iteration=1 --action=clobber --size=$SIZE $val >> output.txt
	runcpu --config=rapid_loop_filer_threshold --noreportable --tune=base --copies=1 --iteration=$ITER --size=$SIZE $val >> output.txt
	
	if [ $val == "508.namd_r" ] || [ $val == "510.parest_r" ] || [ $val == "511.povray_r" ] || [ $val == "519.lbm_r" ] || [ $val == "526.blender_r" ] || [ $val == "538.imagick_r" ] || [ $val == "544.nab_r" ];
    then
    	file1="$SPECDIR/result/CPU2017.002.fprate."$EXT".txt"
		  file2="$SPECDIR/result/CPU2017.004.fprate."$EXT".txt"
		  compute_improvement $file1 $file2 $ITER
    elif [ $val == "500.perlbench_r" ] || [ $val == "502.gcc_r" ] || [ $val == "505.mcf_r" ] || [ $val == "520.omnetpp_r" ] || [ $val == "523.xalancbmk_r" ] || [ $val == "525.x264_r" ] || [ $val == "531.deepsjeng_r" ] || [ $val == "541.leela_r" ] || [ $val == "557.xz_r" ];
    then
    	file3="$SPECDIR/result/CPU2017.002.intrate."$EXT".txt"
		  file4="$SPECDIR/result/CPU2017.004.intrate."$EXT".txt"
		  compute_improvement $file3 $file4 $ITER
    fi
done

#echo "Statistics"
#cd $SPECDIR/ && ./get_stats.sh
