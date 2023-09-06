#!/bin/bash
POLYBENCH=$HOME/artifact_rapid/polybench-c-4.2.1-beta_1
LLVM_ROOT_PATH=$HOME/artifact_rapid/llvm_scout/build
CLANG=$LLVM_ROOT_PATH/bin/clang
EXE_FILES=$POLYBENCH/exe
SCOUT_FLAGS="-O3 -emit-llvm -c -g -mllvm -stats"

BENCHMARK="gesummv 2mm 3mm bicg doitgen jacobi-1d jacobi-2d"
: 'BENCHMARK="correlation covariance gemm gemver gesummv symm syr2k syrk trmm 2mm 3mm atax bicg doitgen mvt cholesky durbin gramschmidt lu ludcmp trisolv deriche floyd-warshall nussinov adi fdtd-2d heat-3d jacobi-1d jacobi-2d seidel-2d"'

for val in $BENCHMARK
do
        if [ $val == "correlation" ] || [ $val == "covariance" ];
    then
        echo $val
        $CLANG $SCOUT_FLAGS -I $POLYBENCH/utilities -I $POLYBENCH/datamining/$val $POLYBENCH/datamining/$val/$val.c -o $EXE_FILES/$val.bc > stats.txt 2>&1
        file="stats.txt"
        loops_benefited=0
        for input in $file
        do
                while IFS= read -r line;
                do
                        if [[ "$line" == *"Number of loops benefited"* ]]; then
                                count=` echo $line | sed 's/[^0-9]*//g'`
                                loops_benefited=`echo "$loops_benefited+$count" | bc`
                        fi
                done < "$input"
        done
        echo "Total number of versioned loops $loops_benefited"
    elif [ $val == "gemm" ] || [ $val == "gemver" ] || [ $val == "gesummv" ] || [ $val == "symm" ] || [ $val == "syr2k" ] || [ $val == "syrk" ] || [ $val == "trmm" ];
    then
        echo $val
        $CLANG $SCOUT_FLAGS -I $POLYBENCH/utilities -I $POLYBENCH/linear-algebra/blas/$val $POLYBENCH/linear-algebra/blas/$val/$val.c -o $EXE_FILES/$val.bc > stats.txt 2>&1
        file="stats.txt"
        loops_benefited=0
        for input in $file
        do
                while IFS= read -r line;
                do
                        if [[ "$line" == *"Number of loops benefited"* ]]; then
                                count=` echo $line | sed 's/[^0-9]*//g'`
                                loops_benefited=`echo "$loops_benefited+$count" | bc`
                        fi
                done < "$input"
        done
        echo "Total number of versioned loops $loops_benefited"
    elif [ $val == "2mm" ] || [ $val == "3mm" ] || [ $val == "atax" ] || [ $val == "bicg" ] || [ $val == "doitgen" ] || [ $val == "mvt" ];
    then
        echo $val
        $CLANG $SCOUT_FLAGS -I $POLYBENCH/utilities -I $POLYBENCH/linear-algebra/kernels/$val $POLYBENCH/linear-algebra/kernels/$val/$val.c -o $EXE_FILES/$val.bc > stats.txt 2>&1
        file="stats.txt"
        loops_benefited=0
        for input in $file
        do
                while IFS= read -r line;
                do
                        if [[ "$line" == *"Number of loops benefited"* ]]; then
                                count=` echo $line | sed 's/[^0-9]*//g'`
                                loops_benefited=`echo "$loops_benefited+$count" | bc`
                        fi
                done < "$input"
        done
        echo "Total number of versioned loops $loops_benefited"
    elif [ $val == "cholesky" ] || [ $val == "durbin" ] || [ $val == "gramschmidt" ] || [ $val == "lu" ] || [ $val == "ludcmp" ] || [ $val == "trisolv" ];
    then
        echo $val
        $CLANG $SCOUT_FLAGS -I $POLYBENCH/utilities -I $POLYBENCH/linear-algebra/solvers/$val $POLYBENCH/linear-algebra/solvers/$val/$val.c -o $EXE_FILES/$val.bc > stats.txt 2>&1
        file="stats.txt"
        loops_benefited=0
        for input in $file
        do
                while IFS= read -r line;
                do
                        if [[ "$line" == *"Number of loops benefited"* ]]; then
                                count=` echo $line | sed 's/[^0-9]*//g'`
                                loops_benefited=`echo "$loops_benefited+$count" | bc`
                        fi
                done < "$input"
        done
        echo "Total number of versioned loops $loops_benefited"
    elif [ $val == "deriche" ] || [ $val == "floyd-warshall" ] || [ $val == "nussinov" ];
    then
        echo $val
        $CLANG $SCOUT_FLAGS -I $POLYBENCH/utilities -I $POLYBENCH/medley/$val $POLYBENCH/medley/$val/$val.c -DPOLYBENCH_TIME -o $EXE_FILES/$val.bc > stats.txt 2>&1
        file="stats.txt"
        loops_benefited=0
        for input in $file
        do
                while IFS= read -r line;
                do
                        if [[ "$line" == *"Number of loops benefited"* ]]; then
                                count=` echo $line | sed 's/[^0-9]*//g'`
                                loops_benefited=`echo "$loops_benefited+$count" | bc`
                        fi
                done < "$input"
        done
        echo "Total number of versioned loops $loops_benefited"
    elif [ $val == "adi" ] || [ $val == "fdtd-2d" ] || [ $val == "heat-3d" ] || [ $val == "jacobi-1d" ] || [ $val == "jacobi-2d" ] || [ $val == "seidel-2d" ];
    then
        echo $val
        $CLANG $SCOUT_FLAGS -I $POLYBENCH/utilities -I $POLYBENCH/stencils/$val $POLYBENCH/stencils/$val/$val.c -o $EXE_FILES/$val.bc > stats.txt 2>&1
        file="stats.txt"
        loops_benefited=0
        for input in $file
        do
                while IFS= read -r line;
                do
                        if [[ "$line" == *"Number of loops benefited"* ]]; then
                                count=` echo $line | sed 's/[^0-9]*//g'`
                                loops_benefited=`echo "$loops_benefited+$count" | bc`
                        fi
                done < "$input"
        done
        echo "Total number of versioned loops $loops_benefited"
    else
            echo "Please enter valid benchmark name."
        fi
done
