#!/bin/bash
file_names="$HOME/artifact_rapid/cpuspec/files_spec.txt"
BENCHMARK="$HOME/artifact_rapid/cpuspec"

while IFS= read -r file;
do
        loops_benefited=0
        regions=0

        for input in $file
        do
                input="$BENCHMARK/benchspec/CPU/$input"
                echo $input
                while IFS= read -r line;
                do
                        if [[ "$line" == *"Number of versioned loops"* ]]; then
                                count=` echo $line | sed 's/[^0-9]*//g'`
                                loops_benefited=`echo "$loops_benefited+$count" | bc`
                        elif [[ "$line" == *"Number of regions required"* ]]; then
                                count=` echo $line | sed 's/[^0-9]*//g'`
                                regions=`echo "$count" | bc`
                        fi
                done < "$input"
        done

        echo $file
        echo "Total number of versioned loops $loops_benefited"
        echo "Total number of regions required $regions"
done < $file_names
