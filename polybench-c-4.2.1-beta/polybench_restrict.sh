#!/bin/bash
POLYBENCH=$HOME/artifact_rapid/polybench-c-4.2.1-beta
LLVM_ROOT_PATH=$HOME/artifact_rapid/llvm_rapid/build
CLANG=$LLVM_ROOT_PATH/bin/clang
EXE_FILES=$POLYBENCH/exe
NATIVE_FLAGS="-O3 -mllvm -disable-additional-vectorize -g"
RESTRICT="-DPOLYBENCH_USE_RESTRICT"

MIMALLCOC="$HOME/artifact_rapid/mimalloc_region"
export LD_LIBRARY_PATH=$MIMALLCOC:$LD_LIBRARY_PATH

MI_LIB_FLAGS="-L$MIMALLCOC -lmimalloc -lm -z muldefs"

iter=5
BENCHMARK="gesummv 2mm 3mm bicg doitgen jacobi-1d jacobi-2d"
#BENCHMARK="correlation covariance gemm gemver gesummv symm syr2k syrk trmm 2mm 3mm atax bicg doitgen mvt cholesky durbin gramschmidt lu ludcmp trisolv deriche floyd-warshall nussinov adi fdtd-2d heat-3d jacobi-1d jacobi-2d seidel-2d"

for val in $BENCHMARK
do
    if [ $val == "correlation" ] || [ $val == "covariance" ];
    then
        echo $val
        $CLANG $NATIVE_FLAGS -I $POLYBENCH/utilities -I $POLYBENCH/datamining/$val $POLYBENCH/utilities/polybench.c $POLYBENCH/datamining/$val/$val.c -DPOLYBENCH_TIME -o $EXE_FILES/$val  $MI_LIB_FLAGS
        extime1=0.0
        for (( i = 0; i < $iter; i++ ))
        do
            sleep 3
            cd $EXE_FILES/ && tmptime=`./$val`
            extime1=`echo "$extime1+$tmptime" | bc`
        done
        average1=`bc <<<"scale=7; $extime1 / $iter"`
            
        $CLANG $NATIVE_FLAGS -I $POLYBENCH/utilities -I $POLYBENCH/datamining/$val $POLYBENCH/utilities/polybench.c $POLYBENCH/datamining/$val/$val.c $RESTRICT -DPOLYBENCH_TIME -o $EXE_FILES/$val  $MI_LIB_FLAGS
        extime2=0.0
        for (( i = 0; i < $iter; i++ ))
        do
            sleep 3
            cd $EXE_FILES/ && tmptime=`./$val`
            extime2=`echo "$extime2+$tmptime" | bc`
        done
        average2=`bc <<<"scale=7; $extime2 / $iter"`
        improvement=`bc <<<"scale=2; ((($average1-$average2)*100) / $average1)"`
        echo $improvement
    elif [ $val == "gemm" ] || [ $val == "gemver" ] || [ $val == "gesummv" ] || [ $val == "symm" ] || [ $val == "syr2k" ] || [ $val == "syrk" ] || [ $val == "trmm" ];
    then
        echo $val
        $CLANG $NATIVE_FLAGS -I $POLYBENCH/utilities -I $POLYBENCH/linear-algebra/blas/$val $POLYBENCH/utilities/polybench.c $POLYBENCH/linear-algebra/blas/$val/$val.c -DPOLYBENCH_TIME -o $EXE_FILES/$val  $MI_LIB_FLAGS
        extime1=0.0
        for (( i = 0; i < $iter; i++ ))
        do
            sleep 3
            cd $EXE_FILES/ && tmptime=`./$val`
            extime1=`echo "$extime1+$tmptime" | bc`
        done
        average1=`bc <<<"scale=7; $extime1 / $iter"`
            
        $CLANG $NATIVE_FLAGS -I $POLYBENCH/utilities -I $POLYBENCH/linear-algebra/blas/$val $POLYBENCH/utilities/polybench.c $POLYBENCH/linear-algebra/blas/$val/$val.c $RESTRICT -DPOLYBENCH_TIME -o $EXE_FILES/$val  $MI_LIB_FLAGS
        extime2=0.0
        for (( i = 0; i < $iter; i++ ))
        do
            sleep 3
            cd $EXE_FILES/ && tmptime=`./$val`
            extime2=`echo "$extime2+$tmptime" | bc`
        done
        average2=`bc <<<"scale=7; $extime2 / $iter"`
        improvement=`bc <<<"scale=2; ((($average1-$average2)*100) / $average1)"`
        echo $improvement
    elif [ $val == "2mm" ] || [ $val == "3mm" ] || [ $val == "atax" ] || [ $val == "bicg" ] || [ $val == "doitgen" ] || [ $val == "mvt" ];
    then
        echo $val
        $CLANG $NATIVE_FLAGS -I $POLYBENCH/utilities -I $POLYBENCH/linear-algebra/kernels/$val $POLYBENCH/utilities/polybench.c $POLYBENCH/linear-algebra/kernels/$val/$val.c -DPOLYBENCH_TIME -o $EXE_FILES/$val  $MI_LIB_FLAGS
        extime1=0.0
        for (( i = 0; i < $iter; i++ ))
        do
            sleep 3
            cd $EXE_FILES/ && tmptime=`./$val`
            extime1=`echo "$extime1+$tmptime" | bc`
        done
        average1=`bc <<<"scale=7; $extime1 / $iter"`
            
        $CLANG $NATIVE_FLAGS -I $POLYBENCH/utilities -I $POLYBENCH/linear-algebra/kernels/$val $POLYBENCH/utilities/polybench.c $POLYBENCH/linear-algebra/kernels/$val/$val.c $RESTRICT -DPOLYBENCH_TIME -o $EXE_FILES/$val  $MI_LIB_FLAGS
        extime2=0.0
        for (( i = 0; i < $iter; i++ ))
        do
            sleep 3
            cd $EXE_FILES/ && tmptime=`./$val`
            extime2=`echo "$extime2+$tmptime" | bc`
        done
        average2=`bc <<<"scale=7; $extime2 / $iter"`
        improvement=`bc <<<"scale=2; ((($average1-$average2)*100) / $average1)"`
        echo $improvement
    elif [ $val == "cholesky" ] || [ $val == "durbin" ] || [ $val == "gramschmidt" ] || [ $val == "lu" ] || [ $val == "ludcmp" ] || [ $val == "trisolv" ];
    then
        echo $val
        $CLANG $NATIVE_FLAGS -I $POLYBENCH/utilities -I $POLYBENCH/linear-algebra/solvers/$val $POLYBENCH/utilities/polybench.c $POLYBENCH/linear-algebra/solvers/$val/$val.c -DPOLYBENCH_TIME -o $EXE_FILES/$val  $MI_LIB_FLAGS
        extime1=0.0
        for (( i = 0; i < $iter; i++ ))
        do
            sleep 3
            cd $EXE_FILES/ && tmptime=`./$val`
            extime1=`echo "$extime1+$tmptime" | bc`
        done
        average1=`bc <<<"scale=7; $extime1 / $iter"`
            
        $CLANG $NATIVE_FLAGS -I $POLYBENCH/utilities -I $POLYBENCH/linear-algebra/solvers/$val $POLYBENCH/utilities/polybench.c $POLYBENCH/linear-algebra/solvers/$val/$val.c $RESTRICT -DPOLYBENCH_TIME -o $EXE_FILES/$val  $MI_LIB_FLAGS
        extime2=0.0
        for (( i = 0; i < $iter; i++ ))
        do
            sleep 3
            cd $EXE_FILES/ && tmptime=`./$val`
            extime2=`echo "$extime2+$tmptime" | bc`
        done
        average2=`bc <<<"scale=7; $extime2 / $iter"`
        improvement=`bc <<<"scale=2; ((($average1-$average2)*100) / $average1)"`
        echo $improvement
    elif [ $val == "deriche" ] || [ $val == "floyd-warshall" ] || [ $val == "nussinov" ];
    then
        echo $val
        $CLANG $NATIVE_FLAGS -I $POLYBENCH/utilities -I $POLYBENCH/medley/$val $POLYBENCH/utilities/polybench.c $POLYBENCH/medley/$val/$val.c -DPOLYBENCH_TIME -o $EXE_FILES/$val  $MI_LIB_FLAGS
        extime1=0.0
        for (( i = 0; i < $iter; i++ ))
        do
            sleep 3
            cd $EXE_FILES/ && tmptime=`./$val`
            extime1=`echo "$extime1+$tmptime" | bc`
        done
        average1=`bc <<<"scale=7; $extime1 / $iter"`
            
        $CLANG $NATIVE_FLAGS -I $POLYBENCH/utilities -I $POLYBENCH/medley/$val $POLYBENCH/utilities/polybench.c $POLYBENCH/medley/$val/$val.c $RESTRICT -DPOLYBENCH_TIME -o $EXE_FILES/$val  $MI_LIB_FLAGS
        extime2=0.0
        for (( i = 0; i < $iter; i++ ))
        do
            sleep 3
            cd $EXE_FILES/ && tmptime=`./$val`
            extime2=`echo "$extime2+$tmptime" | bc`
        done
        average2=`bc <<<"scale=7; $extime2 / $iter"`
        improvement=`bc <<<"scale=2; ((($average1-$average2)*100) / $average1)"`
        echo $improvement
    elif [ $val == "adi" ] || [ $val == "fdtd-2d" ] || [ $val == "heat-3d" ] || [ $val == "jacobi-1d" ] || [ $val == "jacobi-2d" ] || [ $val == "seidel-2d" ];
    then
        echo $val
        $CLANG $NATIVE_FLAGS -I $POLYBENCH/utilities -I $POLYBENCH/stencils/$val $POLYBENCH/utilities/polybench.c $POLYBENCH/stencils/$val/$val.c -DPOLYBENCH_TIME -o $EXE_FILES/$val $MI_LIB_FLAGS
        extime1=0.0
        for (( i = 0; i < $iter; i++ ))
        do
		sleep 3
            cd $EXE_FILES/ && tmptime=`./$val`
	    #echo $tmptime
            extime1=`echo "$extime1+$tmptime" | bc`
        done
        average1=`bc <<<"scale=7; $extime1 / $iter"`

        if [ $val == "jacobi-1d" ] || [ $val == "jacobi-2d" ] ;
        then
            $CLANG $NATIVE_FLAGS -mllvm -handle-higher-order -I $POLYBENCH/utilities -I $POLYBENCH/stencils/$val $POLYBENCH/utilities/polybench.c $POLYBENCH/stencils/$val/$val.c $RESTRICT -DPOLYBENCH_TIME -o $EXE_FILES/$val $MI_LIB_FLAGS
        else
            $CLANG $NATIVE_FLAGS -I $POLYBENCH/utilities -I $POLYBENCH/stencils/$val $POLYBENCH/utilities/polybench.c $POLYBENCH/stencils/$val/$val.c $RESTRICT -DPOLYBENCH_TIME -o $EXE_FILES/$val $MI_LIB_FLAGS
        fi
        extime2=0.0
        for (( i = 0; i < $iter; i++ ))
        do
		sleep 3
            cd $EXE_FILES/ && tmptime=`./$val`
	    #echo $tmptime
            extime2=`echo "$extime2+$tmptime" | bc`
        done
        average2=`bc <<<"scale=7; $extime2 / $iter"`
        improvement=`bc <<<"scale=2; ((($average1-$average2)*100) / $average1)"`
        echo $improvement
    else
            echo "Please enter valid benchmark name."
    fi
done

